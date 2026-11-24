# ESIM

Simulator für analoge Schaltungen und differential-algebraische Gleichungen
(DAE) in C++17. Die Eingabe ist eine JSON-Datei, die Ergebnisse sind CSV.

Simuliert werden

- **Schaltungen** aus Widerständen, Kondensatoren, Spulen, Quellen, Dioden,
  Bipolartransistoren, MOSFETs, idealen Schaltern und idealen Dioden,
- **Gleichungssysteme**, die direkt in der Eingabedatei stehen: gewöhnliche
  Differentialgleichungen mit algebraischen Nebenbedingungen,
- **Ereignisse**: Umschaltungen und Sprünge, die vom Zustand abhängen
  (Schalter, Anschläge, Hysterese),
- und **alles gekoppelt** in einem System (z. B. Elektrik + Thermik + Regler).

Der numerische Kern löst allgemein

    d/dt q(x) + f(x, t) = 0

und weiß nichts von Schaltungen. Zeilen mit `q_i = 0` sind algebraische
Bedingungen, alle anderen Differentialgleichungen. Eine Schaltung liefert `f`
(Ströme) und `q` (Ladungen, Flüsse) über die modifizierte Knotenanalyse, ein
Gleichungsblock liefert sie aus dem Text der Gleichungen.

**Benutzerhandbuch:** [`docs/handbuch/handbuch.pdf`](docs/handbuch/handbuch.pdf)
(rund 100 Seiten: Installation, erste Schritte, Editor, vollständige
Beschreibung des Eingabeformats, Kommandozeile, Python, SPICE, Fehlersuche).
Quelltext in LaTeX unter [`docs/handbuch/`](docs/handbuch); übersetzen mit
`cd docs/handbuch && latexmk -pdf handbuch.tex`.

**Geplant:** ein Halbleitersimulator als Plugin (Drift-Diffusion auf einem
Gitter, für Optoelektronik, Hall- und chemische Sensoren), Spezifikation in
[`docs/halbleiter-plugin.md`](docs/halbleiter-plugin.md).

## Bauen und Starten

Voraussetzungen: CMake ≥ 3.18, ein C++17-Compiler. Eigen und nlohmann/json
werden verwendet, wenn sie installiert sind (`brew install eigen nlohmann-json`),
sonst lädt CMake sie herunter.

    cmake -S . -B build
    cmake --build build
    ./build/esim_tests                    # Testsuite (oder: cd build && ctest)

    ./build/esim examples/bjt_amplifier.json -o results

**Windows 11** (Visual Studio 2022 oder dessen Build Tools mit „Desktop
development with C++“, CMake, Qt 6 für MSVC 2022 64-bit aus dem Qt-Installer,
optional Python 3 für die Tests; in der „x64 Native Tools Command Prompt“):

    cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
    cmake --build build --config Release
    ctest --test-dir build -C Release
    cmake --install build --config Release --prefix dist   # Editor mit esim, Qt-DLLs, Beispielen

CMake nimmt das neueste installierte Visual Studio (2022 oder neuer); Qts
Bibliotheken für MSVC 2022 passen auch zu neueren Versionen. `dist` läuft
dann allein (`dist\esim-editor.exe`). Ohne Qt wird nur der
Simulator gebaut. Die Programme verwenden intern UTF-8 (Manifest), Dateien
und Ordner mit Umlauten gehen also. Im Editor heißen die Kürzel unter
Windows Strg und Alt statt ⌘ und ⌥. Ein Arbeitsablauf für GitHub Actions
(`.github/workflows/build.yml`) baut und testet auf Windows und macOS.

Der Arbeitspunkt wird auf der Konsole ausgegeben, alle anderen Analysen landen
in `<Ausgabeordner>/<Dateiname>.<Analyse>.csv`, z. B.
`results/bjt_amplifier.tran.csv`.

Parameterstudien (Sweeps, Monte Carlo, Empfindlichkeiten, Fit) rechnen ihre
Läufe gleichzeitig auf allen Prozessorkernen, einzelne Simulationen großer
Schaltungen (ab 1000 Bauteilen) werten ihre Bauteile parallel aus; `-j <n>`
begrenzt die Zahl der Threads. Die Ergebnisse hängen nicht von dieser Zahl ab.

Beispiele in [`examples/`](examples):

| Datei | Inhalt |
|-------|--------|
| `rc_lowpass.json` | Arbeitspunkt, Frequenzgang, Pulsantwort |
| `rlc_ringing.json` | gedämpfte Schwingung aus Anfangsbedingungen |
| `bridge_rectifier.json` | Brückengleichrichter mit Z-Diode |
| `bjt_amplifier.json` | Emitterschaltung: Arbeitspunkt, AC, Transient |
| `cmos_inverter.json` | Übertragungskennlinie (DC-Sweep) und Schaltvorgang |
| `astable_multivibrator.json` | Kippschwinger mit zwei Transistoren |
| `dae_robertson.json` | steifes DAE ohne Schaltung (Reaktionskinetik) |
| `vanderpol.json` | nichtlinearer Oszillator als Gleichungssystem |
| `ntc_inrush.json` | Schaltung und Gleichungen gekoppelt: Heißleiter mit Eigenerwärmung |
| `bouncing_ball.json` | Ereignis mit Zustands-Reset |
| `thermostat.json` | Zweipunktregler mit Hysterese (diskrete Variable), Ausgabe auf festem Raster |
| `relaxation_oscillator.json` | Schaltung mit Ereignissen (Prinzip des 555), Parameter-Sweep |
| `buck_converter.json` | Tiefsetzsteller mit idealem Schalter und idealer Diode |
| `opamp_filter.json` | aktives Filter aus verschachtelten Subcircuits, Messgrößen |
| `transformer_supply.json` | Transformator (gekoppelte Spulen), Brückengleichrichter, Messgrößen |
| `solar_cell.json` | Kennlinie aus einer Tabelle, Punkt maximaler Leistung per DC-Sweep |
| `digital_pi_control.json` | abgetasteter PI-Regler (Zeitereignisse) an einer Regelstrecke |
| `pendulum.json` | Mechanik mit Zwangsbedingung (DAE vom Index 2) |
| `monte_carlo_filter.json` | Toleranzanalyse (Monte Carlo) und verschachtelter Sweep |
| `fit_diode.json` | Modellparameter an eine gemessene Kennlinie anpassen |
| `sensitivity.json` | Empfindlichkeiten von Arbeitspunkt und Verstärkung einer Transistorstufe |
| `mosfet_subthreshold.json` | EKV-Modell und Level 1 im Vergleich, Kennlinie über acht Dekaden |
| `transmission_line.json` | Reflexionen auf einer fehlangepassten Leitung |
| `dead_time_control.json` | Regelkreis mit Totzeit (`delay`) |
| `heat_rod.json` | Wärmeleitung als Vektor von Gleichungen (Linienmethode) |
| `pendulum_index3.json` | Pendel mit Lagebedingung (Index 3), automatisch reduziert |
| `ring_oscillator_bsim3.json` | Ringoszillator mit BSIM3-Transistoren (0,18 µm), Modellkarten im SPICE-Format |
| `saturating_inductor.json` | nichtlineare Induktivität über ihren Fluss, eigene Funktion |

### Ergebnisse plotten

`tools/plot.py` zeichnet die CSV-Dateien mit matplotlib (`pip install matplotlib`):

    python3 tools/plot.py results/buck_converter.tran.csv
    python3 tools/plot.py results/bjt_amplifier.ac.csv -o bode.png
    python3 tools/plot.py results/relaxation_oscillator.sweep.csv --cols "v\(c\)"

Die Darstellung richtet sich nach der Datei: Zeitverläufe mit getrennten Feldern
für Spannungen, Ströme und übrige Größen, Frequenzgänge als Betrag in dB und
(entfaltete) Phase über logarithmischer Frequenz, DC-Sweeps über der ersten
Spalte, Parameter-Sweeps mit einer Kurve je Parameterwert. `--cols` wählt
Spalten per regulärem Ausdruck, `--separate` gibt jeder Größe ein eigenes Feld,
`-o` schreibt ein Bild statt ein Fenster zu öffnen. Mehrere Dateien werden
übereinandergelegt.

### Aus Python

`python/esim.py` startet den Simulator und liefert die Ergebnisse als
Datenstrukturen (numpy-Arrays, falls numpy installiert ist, sonst Listen). Es
braucht nur die Standardbibliothek:

```python
import sys; sys.path.insert(0, "python")
import esim

res = esim.run("examples/opamp_filter.json")
res.ac.measures["f3db"]                       # 999.97
f, mag = res.ac["freq"], res.ac["mag(v(out))"]

# Netzliste als dict, Parameter überschreiben, Analysen ersetzen:
res = esim.run(netlist, parameters={"C": 220e-9},
               analyses=[{"type": "tran", "tstop": "5m"}])
t, v = res.tran["time"], res.tran["v(out)"]
```

- `res.op`, `res.dc`, `res.ac`, `res.tran`, `res.pss`, `res.noise`, `res.sens`
  sind die erste Analyse des Typs, `res["name"]` eine benannte,
  `res.analyses` alle.
- Monte Carlo liefert `statistics`, ein Fit `parameters`, die
  Empfindlichkeitsanalyse `sensitivities`; `info` enthält alles, was der
  Simulator zu der Analyse gemeldet hat.
- `threads=n` begrenzt die Zahl gleichzeitiger Läufe einer Parameterstudie.
- Jede Analyse hat `data` (Spalten, auch per `analyse["spalte"]`), `measures`,
  `values` (Arbeitspunkt) und `stats`. Bei einem Parameter-Sweep enthält
  `measures` je Messung eine Liste, und `runs()` liefert die Kurven je
  Parameterwert.
- Fehler des Simulators kommen als `esim.EsimError` mit dessen Meldung an.
- Das Programm wird über das Argument `esim=`, die Umgebungsvariable
  `ESIM_BIN`, `build/esim` neben dem Modul oder den Suchpfad gefunden.

Für eigene Programme schreibt `esim netz.json --summary ergebnis.json` eine
maschinenlesbare Übersicht aller Analysen (Dateien, Arbeitspunktwerte,
Messgrößen, Statistik). `--progress` meldet während Transienten und
Parameterstudien Zeilen `progress <Analyse> <Anteil>` auf stderr.

### Editor

`esim-editor` (Qt 6, wird mitgebaut, wenn CMake Qt ≥ 6.5 findet, z. B. nach
`brew install qt`) ist eine Oberfläche für die Eingabedateien:

    open build/editor/esim-editor.app                       # macOS
    build/editor/esim-editor.app/Contents/MacOS/esim-editor examples/rc_lowpass.json

- Schaltplan und Text nebeneinander, beide bearbeitbar. Der Text bleibt
  maßgeblich: Der Plan steht unter `"schematic"` in derselben Datei (der
  Simulator überliest ihn), und jede Änderung im Plan wird als kleine
  Textänderung geschrieben – Kommentare und Aufbau der Datei bleiben, ⌘Z
  wirkt auf beides. Knotennamen ergeben sich aus Leitungen, Netzlabels und
  Masse. Dateien ohne Plan werden automatisch angeordnet, mit einem Label
  an jedem Anschluss.
- Plan: Bauteile aus der Liste „Bauteile“ (oder Taste A) platzieren;
  R dreht, X spiegelt, ⌫ löscht, ⌘D verdoppelt, Pfeiltasten verschieben;
  W zeichnet Leitungen (enden an Anschluss oder Leitung), G setzt Masse,
  N ein Netzlabel; Rechtsklick öffnet ein Menü. Leitungen an verschobenen
  Bauteilen gehen mit. Die Auswahl im Plan markiert das Bauteil im Text
  und umgekehrt; „Eigenschaften“ zeigt seine Werte als Formular.
  Teilschaltungen haben einen eigenen Plan: Doppelklick auf eine Instanz
  öffnet ihn, ⌘↑ führt zurück; Ports (Taste P) verbinden mit außen, ein
  neuer Port-Name erweitert die Ports. „Neue Teilschaltung …“ in der
  Bauteilliste legt eine an. ⌘G macht aus den ausgewählten Bauteilen (mit ihren
  Leitungen) eine Teilschaltung: Ports werden die Netze, die nach außen
  führen; an ihrer Stelle steht eine Instanz, Labels halten die
  Verbindungen. Steht der Textcursor in einem Bauteil einer
  Teilschaltung, zeigt der Plan diese.
- Modellbibliothek (⌘L, oder Suche in „Bauteile“, z. B. „NPN_GEN“): Modelle
  aus der Standardbibliothek, aus den Beispielen und aus eigenen
  Bibliotheken (SPICE-Dateien der Hersteller mit `.model`-Karten und
  `.subckt`-Teilschaltungen, oder
  esim-Dateien, einzeln oder ganze Ordner). Ein gewähltes Modell wird in
  `"models"` der Datei kopiert, die damit vollständig bleibt. Im Feld
  „model“ der Eigenschaften schlägt der Editor passende Modelle vor; „…“
  öffnet die Bibliothek. Die Standardbibliothek
  ([`editor/library/standard.lib`](editor/library/standard.lib)) enthält
  allgemeine Lehrbuchmodelle; Bauteile bestimmter Hersteller kommen aus
  deren SPICE-Dateien.
  Nach einer Simulation stehen die Spannungen des Arbeitspunkts an den
  Labels; ⌥-Klick auf eine Leitung oder ein Bauteil zeigt Spannung oder
  Strom als Kurve.
- Texteditor mit Hervorhebung, Zeilennummern und Einrückung; die Eingabe wird
  beim Tippen mit dem Parser des Simulators geprüft, Fehler und Warnungen
  stehen an ihrer Zeile und in der Liste „Probleme“.
- ⌘R sichert und startet die Simulation (als eigener Prozess, ⌘. bricht ab),
  mit Fortschrittsanzeige und Protokoll.
- Ergebnisse: Analysen, Größen zum Ankreuzen, Werte (Arbeitspunkt,
  Messgrößen) als Tabelle und Kurven in Feldern mit gemeinsamer x-Achse.
  Scrollen oder Spreizen zoomt in x (mit ⌘ in y), Ziehen zoomt auf ein
  Rechteck, Doppelklick zeigt alles; das Fadenkreuz zeigt die Werte.
  Frequenzgänge erscheinen als Betrag in dB und Phase, Sweeps mit einer
  Farbstufe je Lauf.
- Messgrößen über viele Läufe: bei Monte Carlo ein Histogramm je Größe
  (Mittelwert, ±σ, Statistik) oder die Punkte über einem gestreuten
  Parameter; bei Parameter-Sweeps die Messgröße über einem Parameter,
  verschachtelte Sweeps als Kurvenschar (Auswahl „Darstellung“).
- Konzept, Stand und nächste Schritte: [`docs/editor.md`](docs/editor.md).

Der Editor bringt das Programm `esim` in seinem Paket mit.
`esim-editor datei.json --run --snapshot bild.png` rechnet und speichert das
Fenster als Bild (für Tests und Dokumentation); `--subcircuit name` zeigt
dabei den Plan einer Teilschaltung.

### SPICE-Netzlisten übernehmen

`tools/spice2json.py` übersetzt eine SPICE-Netzliste in das JSON-Format:

    python3 tools/spice2json.py examples/spice/amplifier.cir -o amplifier.json
    ./build/esim amplifier.json

Übersetzt werden die Bauteile R, C, L, K, V, I, E, G, F, H, D, Q, M, J, X, S,
T und O (verlustbehaftete Leitung mit LTRA-Modell), die Quellenformen DC, AC,
SIN, PULSE und PWL sowie `.model`, `.subckt`, `.param`, `.op`, `.dc`, `.ac`, `.tran`, `.noise`, `.four`, `.ic`, `.nodeset`,
`.temp`, `.options` und `.print`. Namen und Knoten werden kleingeschrieben
(SPICE unterscheidet keine Groß- und Kleinschreibung). Was nicht übersetzt
werden kann, wird gemeldet und ausgelassen; eingebundene Dateien (`.include`,
`.lib`) werden nicht gelesen.

## Eingabeformat

Kommentare (`//`, `/* */`) sind erlaubt. Unbekannte Schlüssel erzeugen eine
Warnung. Eine Datei enthält `components`, `equations` oder beides.

```jsonc
{
  "title": "Einweggleichrichter",

  "models": {
    "DGEN": { "type": "diode", "is": 2.5e-9, "n": 1.75, "cj0": "4p", "tt": "20n" }
  },

  "components": [
    { "type": "V", "name": "V1", "nodes": ["in", "0"],
      "wave": { "type": "sin", "amplitude": 5, "freq": "1k" } },
    { "type": "D", "name": "D1", "nodes": ["in", "out"], "model": "DGEN" },
    { "type": "R", "name": "R1", "nodes": ["out", "0"], "value": "10k" },
    { "type": "C", "name": "C1", "nodes": ["out", "0"], "value": "1u" }
  ],

  "analyses": [
    { "type": "op" },
    { "type": "tran", "tstop": "5m" }
  ],

  "outputs": ["v(in)", "v(out)", "i(V1)"]
}
```

### Zahlen und Parameter

Ein Zahlenwert ist eine JSON-Zahl, ein String mit SI-Präfix und optionaler
Einheit (`"4.7k"`, `"100nF"`, `"2.2M"`) oder ein Ausdruck aus Parametern
(`"2*R0"`, `"1/(2*pi*R0*C0)"`). Die Präfixe unterscheiden Groß-/Kleinschreibung
(anders als in SPICE):

| f | p | n | u, µ | m | k, K | M, meg | G | T |
|---|---|---|------|---|------|--------|---|---|
| 1e-15 | 1e-12 | 1e-9 | 1e-6 | 1e-3 | 1e3 | 1e6 | 1e9 | 1e12 |

Buchstaben nach dem Präfix gelten als Einheit und werden ignoriert (`"1F"` ist
1, nicht 1 Femto). In Ausdrücken ist nur das Präfix erlaubt (`4.7k*2`), keine
Einheit.

Parameter sind benannte Konstanten. Sie dürfen sich aufeinander beziehen, die
Reihenfolge spielt keine Rolle:

```jsonc
"parameters": { "R0": "1k", "C0": "100n", "tau": "R0*C0" }
```

### Ausdrücke

Ausdrücke stehen in Gleichungen, Verhaltensquellen, Ausgängen und überall, wo
eine Zahl erwartet wird (dort nur mit Parametern).

| | |
|---|---|
| Operatoren | `+ - * /`, Potenz `^` oder `**`, Vergleiche `< <= > >= == !=`, Logik `&& \|\| !` |
| Funktionen | `sin cos tan asin acos atan atan2(y,x) sinh cosh tanh exp log ln log10 sqrt abs sign min max pow(a,b)` |
| Fallunterscheidung | `if(bedingung, dann, sonst)` |
| Tabellen | `name(x)` für eine unter `tables` definierte Kennlinie |
| eigene Funktionen | `name(a, b)` für eine unter `functions` definierte Funktion |
| Totzeit | `delay(x, td)`: der Wert, den `x` vor der Zeit `td` hatte |
| Vektoren | `T[i]`: Element eines Vektors; `sum(i, von, bis, ausdruck)`: Summe |
| Namen | Variablen, Parameter, `t` (oder `time`), `pi` |
| Schaltungsgrößen | `v(knoten)`, `v(a,b)`, `i(bauteil)` |
| Zeitableitung | `der(ausdruck)`, nur in Gleichungen |

Die Ableitungen für das Newton-Verfahren werden automatisch und exakt gebildet.
Unstetige Ausdrücke (Vergleiche, `if`, `abs`, `sign`, `min`, `max`) sind in
Gleichungen und Verhaltensquellen erlaubt: Die Stelle, an der ein Vergleich
kippt, wird als [Ereignis](#ereignisse) exakt bestimmt.

### Eigene Funktionen

Wiederkehrende Ausdrücke lassen sich als Funktionen ablegen und dann in
Gleichungen, Verhaltensquellen, Schaltern, Ereignissen und Ausgängen aufrufen:

```jsonc
"functions": {
  "clip(x, lim)":  "max(-lim, min(lim, x))",
  "core(i, L, is)": "L*is*tanh(i/is)"
}
```

Ein Aufruf wird durch den Funktionsrumpf ersetzt, mit den Argumenten anstelle
der Parameter. Funktionen dürfen Parameter der Eingabedatei und andere
Funktionen verwenden; sie dürfen sich nicht selbst aufrufen und keine
eingebaute Funktion überschreiben. In Zahlenwerten (Bauteilwerte, `parameters`)
stehen sie nicht zur Verfügung.

### Totzeiten

`delay(x, td)` liefert den Wert, den der Ausdruck `x` vor der konstanten Zeit
`td` hatte. Damit lassen sich Transportvorgänge, Rechenzeiten eines Reglers
oder Differentialgleichungen mit Gedächtnis schreiben:

```jsonc
"equations": [ "T * der(y) = K * delay(u, Td) - y" ]
```

- Vor dem Start gilt der Wert bei t = 0 (das System war in Ruhe).
- Im Arbeitspunkt und in der AC-Analyse ist `delay(x, td)` gleich `x`.
- Kein Zeitschritt ist länger als die kleinste Totzeit. Zwischen den
  gespeicherten Zeitpunkten wird kubisch interpoliert, an Knicken linear;
  Knicke und Sprünge setzen eine Totzeit später einen Haltepunkt.
- `td` muss eine positive Konstante sein (Zahl oder Parameter).

`examples/dead_time_control.json` zeigt einen Regelkreis mit Totzeit. Die
Analyse `pss` unterstützt keine Totzeiten und Leitungen.

### Tabellen

Kennlinien aus Stützstellen, etwa Messdaten, werden unter `tables` definiert
und in Ausdrücken wie eine Funktion aufgerufen:

```jsonc
"tables": {
  "cell": { "x": [0, 0.3, 0.5, 0.6], "y": [3.0, 2.96, 2.38, 0], "interp": "cubic" },
  "ntc":  { "file": "ntc_curve.csv", "xcol": 0, "ycol": 1 }
},
"components": [
  { "type": "B", "name": "Bcell", "nodes": ["0", "p"], "i": "cell(v(p))" }
]
```

- Daten: `x` und `y`, oder `points` als Liste von `[x, y]`, oder `file`: eine
  Textdatei mit Zahlenspalten (Komma, Semikolon oder Leerzeichen als Trenner;
  Zeilen, die nicht mit einer Zahl beginnen, werden übersprungen). Der Pfad
  gilt relativ zur Eingabedatei, `xcol` und `ycol` wählen die Spalten (ab 0).
  `x` muss streng steigend sein.
- `interp`: `linear` (Standard) oder `cubic`. Die kubische Interpolation ist
  monoton (PCHIP): glatte Steigung, aber kein Überschwingen zwischen den
  Stützstellen. Für Kennlinien in Gleichungen und Quellen ist sie meist die
  bessere Wahl, weil das Newton-Verfahren dann eine stetige Ableitung sieht.
- `extrap`: außerhalb der Daten wird der Randwert gehalten (`hold`, Standard)
  oder mit der Randsteigung fortgesetzt (`linear`).

### Gleichungen

```jsonc
"variables": {
  "y1": 1,                               // Anfangswert
  "y2": { "init": 0, "abstol": 1e-12 },  // Anfangswert mit eigener Toleranz
  "y3": { "guess": 0.5 }                 // nur Startwert für den Löser
},
"equations": [
  "der(y1) = -0.04*y1 + 1e4*y2*y3",
  "der(y2) =  0.04*y1 - 1e4*y2*y3 - 3e7*y2^2",
  "0 = y1 + y2 + y3 - 1"
]
```

Es braucht genauso viele Gleichungen wie Variablen. Eine Gleichung ohne `der()`
ist eine algebraische Bedingung. `der()` darf einen beliebigen Ausdruck der
Unbekannten einschließen, muss aber linear mit konstantem Faktor in der
Gleichung stehen:

    m*der(v) = F - k*x                  ok
    der(C0*u + C1*u^2) = -u/R           ok (nichtlineare "Ladung")
    der(x) + der(y) = -x                ok
    x*der(y) = 1                        nicht erlaubt -> Hilfsvariable einführen:
                                        der(y) = dy  und  x*dy = 1

Gleichungen höherer Ordnung werden als System erster Ordnung geschrieben
(`der(x) = v`, `der(v) = ...`).

**Anfangswerte.** Eine Variable, die in einem `der()` vorkommt, ist ein Zustand.

- Zahl oder `"init"`: Der Zustand startet bei diesem Wert.
- `"guess"` oder `null`: Der Zustand startet im Gleichgewicht (seine
  Zeitableitung ist bei t = 0 null); der Wert ist nur Startwert für Newton.
- Für algebraische Variablen ist jeder Wert nur ein Startwert: Vor der
  Integration werden sie so bestimmt, dass alle algebraischen Gleichungen
  erfüllt sind (konsistente Anfangswerte).

Die Analysen `op`, `dc` und `ac` rechnen immer mit dem Gleichgewicht aller
Gleichungen; die vorgegebenen Anfangswerte gelten nur für `tran`.

### Vektoren

Viele gleichartige Unbekannte, etwa die Stützstellen einer örtlich
diskretisierten partiellen Differentialgleichung, werden als Vektor
vereinbart; eine Schleife schreibt ihre Gleichungen:

```jsonc
"parameters": { "n": 41, "a": 1.1e-4, "dx": "0.2/(n-1)" },
"variables":  { "T[n]": 20 },
"equations": [
  "T[1] = 100",
  { "for": "i", "from": 2, "to": "n-1",
    "equation": "der(T[i]) = a * (T[i-1] - 2*T[i] + T[i+1]) / dx^2" },
  "T[n] = T[n-1]"
]
```

- `"T[n]"` legt die Unbekannten `T[1]` bis `T[n]` an. Der Startwert ist eine
  Zahl für alle Elemente oder eine Liste mit einem Wert je Element
  (`"init": [..]`, `"guess": [..]`).
- Eine Schleife `{"for": "i", "from": .., "to": .., "equation": ".."}` steht
  für eine Gleichung je Indexwert. Mit `"equations": [...]` enthält sie mehrere
  Einträge, die wieder Schleifen sein dürfen.
- Der Index ist ein konstanter ganzzahliger Ausdruck aus Zahlen, Parametern
  und Schleifenindizes. Ein Index, der von einer Unbekannten abhängt, ist
  nicht möglich.
- `sum(i, von, bis, ausdruck)` summiert über einen Index.
- In den Ausgängen steht `T[*]` für alle Elemente.

Die Schleifen werden beim Einlesen ausgerollt; für den Löser sind es einzelne
Gleichungen. Matrix- und Vektoroperationen (Skalarprodukt, Matrix mal Vektor)
gibt es nicht; sie lassen sich mit `sum` schreiben. Beispiel:
`examples/heat_rod.json`.

### Indexreduktion

Zwangsbedingungen der Mechanik und vorgegebene Bahnen führen auf Systeme, die
ein Integrationsverfahren nicht direkt lösen kann. Das Pendel in kartesischen
Koordinaten ist das Standardbeispiel:

```jsonc
"equations": [
  "der(x) = u", "der(y) = v",
  "der(u) = -2*x*lam", "der(v) = -g - 2*y*lam",
  "0 = x^2 + y^2 - L^2"          // Index 3
]
```

Beim Einlesen bestimmt eine Strukturanalyse (Algorithmus von Pantelides), wie
oft jede Gleichung abgeleitet werden müsste. Ist der Index größer als 2, wird
das System selbsttätig umgeformt:

- Jede algebraische Bedingung `g = 0`, die k-mal abzuleiten wäre, erhält ihre
  Zeitableitungen bis zur Ordnung k−1 als zusätzliche Gleichungen. Sie werden
  symbolisch gebildet, wobei für `der(x)` die rechten Seiten der Gleichungen
  `c*der(x) = ...` eingesetzt werden.
- Damit Bedingung und Ableitungen zugleich gelten können, kommt je zusätzlicher
  Gleichung ein Multiplikator `index_mu1`, `index_mu2`, … als neue Unbekannte
  hinzu. Er wirkt in den Gleichungen der Unbekannten der Bedingung als
  `der(x) = ... − ∂g/∂x · mu` (Stabilisierung nach Gear, Gupta und Leimkuhler)
  und ist für die exakte Lösung null.
- Das Ergebnis hat Index 2. Bedingung und Ableitungen gelten mit der
  Genauigkeit der Newton-Iteration, es gibt keine Drift.

Das Programm meldet, was es hinzugefügt hat:

    note: index 3 reduced to 2 by adding time derivatives of constraints:
      "0 = x^2 + y^2 - L^2":
        added  0 = 2*x*u + 2*y*v   (multiplier index_mu1 in the equations of x, y)

Das ist genau das System, das `examples/pendulum.json` von Hand ausschreibt;
`examples/pendulum_index3.json` rechnet die obige Form und kommt auf dieselben
614 Schritte und dieselbe Periode. Bedingungen dürfen explizit von der Zeit
abhängen (bewegter Aufhängepunkt, vorgegebene Bahn). Ein Test mit Index 4
(drei Integratoren, deren erster Zustand vorgegeben ist) findet die gesuchte
Eingangsgröße auf 1e-6 genau.

Voraussetzungen und Grenzen: siehe „Grenzen“ unter Index.
`"options": {"index_reduction": false}` schaltet die Umformung ab; dann warnt
die Transientenanalyse nur.

### Ereignisse

**Automatisch.** Jeder Vergleich (`<`, `<=`, `>`, `>=`) und jedes `abs`, `sign`,
`min`, `max` in Gleichungen und Verhaltensquellen wird überwacht. Kippt eines
davon während eines Zeitschritts, sucht der Löser den Zeitpunkt, endet dort,
schaltet um und startet die Integration neu. Bis dahin rechnet er mit dem alten
Zweig weiter, sodass z. B. `der(z) = if(t < 1, 1, -2)` exakt bei t = 1 knickt.
Dafür ist nichts weiter anzugeben.

**Mit Aktion.** Ein Eintrag unter `events` weist Zuständen neue Werte zu. Er
hat genau einen Auslöser:

| Auslöser | feuert |
|----------|--------|
| `"when": "h < 0"` | in dem Moment, in dem die Bedingung wahr *wird* |
| `"every": "1m"` | periodisch; erstmals nach einer Periode oder bei `"start"` |
| `"at": 2.5` oder `"at": [1, 2.5]` | zu den angegebenen Zeiten |

Ein Beispiel mit Bedingung:

```jsonc
"variables": { "h": 1, "v": 0 },
"equations": [ "der(h) = v", "der(v) = -g" ],
"events": [
  { "when": "h < 0", "set": { "v": "-0.8*v" } }      // springender Ball
]
```

- Die rechten Seiten von `set` werden mit den Werten vor dem Ereignis
  berechnet, erst dann wird zugewiesen. Löst eine Zuweisung weitere Bedingungen
  aus, feuern diese im selben Zeitpunkt.
- Zuweisen lassen sich Zustände (Variablen in einem `der()`), diskrete
  Variablen sowie Größen der Schaltung: `"v(c)"` für die Spannung eines
  Knotens mit Kondensator, `"i(L1)"` für einen Spulenstrom. Algebraische
  Größen ergeben sich nach dem Ereignis neu aus den Gleichungen; eine
  Zuweisung an sie bleibt wirkungslos.
- Zeitereignisse werden exakt getroffen. Mit `every` entsteht ein abgetastetes
  System: Ein Regler liest zu festen Zeiten die Schaltung aus und ändert seine
  diskreten Zustände, dazwischen läuft die Strecke kontinuierlich
  (`examples/digital_pi_control.json`).
- Eine Bedingung, die schon beim Start wahr ist, löst nicht aus.
- In der Ausgabe steht am Ereigniszeitpunkt der Zustand *vor* der Zuweisung,
  der nächste Punkt (unmittelbar danach) zeigt den neuen.

**Diskrete Variablen** ändern sich nur durch Zuweisungen und sind dazwischen
konstant: Schaltzustände, Betriebsarten, Zähler. Sie brauchen keine Gleichung:

```jsonc
"variables": { "T": 15, "heat": { "discrete": 1 } },
"equations": [ "Cth*der(T) = P*heat - (T - Tout)/Rth" ],
"events": [
  { "when": "T > 22", "set": { "heat": 0 } },         // Zweipunktregler
  { "when": "T < 20", "set": { "heat": 1 } }          // mit Hysterese
]
```

In `op`, `dc` und `ac` behalten diskrete Variablen ihren Startwert.

**Schalter und ideale Dioden** sind Ereignisse ohne weitere Angaben, ebenso
Vergleiche in Verhaltensquellen.

**Grenzen.** Ein Modell, das ohne Hysterese hin- und herschaltet
(`der(x) = if(x < 1, 1, -1)` pendelt um x = 1) oder dessen Ereignisse sich
häufen (der Ball springt unendlich oft in endlicher Zeit), bricht nach
`max_events` Ereignissen mit einer Fehlermeldung ab. Der Löser sucht
Vorzeichenwechsel am Ende und an drei Stellen im Inneren jedes Zeitschritts;
ein Hin- und Rückwechsel, der kürzer als ein Viertelschritt ist, kann
unbemerkt bleiben. `tstep` begrenzt die Schrittweite.

### Kopplung mit der Schaltung

Gleichungen lesen die Schaltung über `v(...)` und `i(...)`. In die andere
Richtung wirkt das Verhaltensbauteil `B`, dessen Strom, Spannung oder Ladung
ein Ausdruck ist, der auch Variablen enthalten darf:

```jsonc
"variables": { "T": 298.15, "Rntc": { "guess": 10 } },
"equations": [
  "Cth*der(T) = v(in,mid)^2/Rntc - (T - Tamb)/Rth",     // Wärmebilanz
  "Rntc = R25*exp(B*(1/T - 1/T0))"                       // Kennlinie
],
"components": [
  { "type": "B", "name": "NTC", "nodes": ["in", "mid"], "i": "v(in,mid)/Rntc" }
]
```

Das vollständige Beispiel ist `examples/ntc_inrush.json`.

### Knoten

Knotennamen sind beliebige Strings (oder ganze Zahlen). Masse heißt `"0"` oder
`"gnd"`. Jede Schaltung braucht einen Massebezug, und jeder Knoten braucht einen
Gleichstrompfad nach Masse. In Ausdrücken sind nur Knotennamen ohne Komma und
Klammern verwendbar.

### Bauteile

`type` akzeptiert den SPICE-Buchstaben oder den langen Namen. `name` ist
optional, wird aber für `i(...)`, `dc`-Sweeps und gesteuerte Quellen gebraucht.
Schlüssel von Bauteilen und Modellen sind unabhängig von Groß-/Kleinschreibung.

| type | Bauteil | nodes | Parameter |
|------|---------|-------|-----------|
| `R`, `resistor` | Widerstand | a, b | `value`; `"noise": false` ohne thermisches Rauschen |
| `C`, `capacitor` | Kondensator | a, b | `value`, `ic` (Spannung a−b bei t=0) |
| `L`, `inductor` | Spule | a, b | `value`, `ic` (Strom a→b bei t=0) |
| `V`, `vsource` | Spannungsquelle | +, − | `dc`, `wave`, `ac`, `acphase` |
| `I`, `isource` | Stromquelle (Strom von Knoten 1 durch die Quelle nach Knoten 2) | von, nach | `dc`, `wave`, `ac`, `acphase` |
| `E`, `vcvs` | spannungsgesteuerte Spannungsquelle | +, −, Steuer+, Steuer− | `gain` |
| `G`, `vccs` | spannungsgesteuerte Stromquelle | von, nach, Steuer+, Steuer− | `gm` |
| `F`, `cccs` | stromgesteuerte Stromquelle | von, nach | `control` (Name einer V-Quelle/Spule), `gain` |
| `H`, `ccvs` | stromgesteuerte Spannungsquelle | +, − | `control`, `r` |
| `B`, `behavioral` | Verhaltensquelle | +, − | genau eines von `i`, `v`, `q`, `phi` (Ausdruck) |
| `J`, `njf`, `pjf` | Sperrschicht-FET | D, G, S | `vto` `beta` `lambda` `is` `cgs` `cgd` `pb` `fc` `kf` `af` |
| `K`, `coupling` | magnetische Kopplung | – | `inductors`: zwei Spulennamen, `k` (−1 < k < 1) |
| `T`, `tline` | Leitung | Tor 1 (+, −), Tor 2 (+, −) | `z0`, `td` (oder `f` und `nl` wie in SPICE, oder `l`, `c` je Länge mit `len`); Verluste `r`, `g` je Länge, `segments` |
| `X`, `subcircuit` | Instanz eines Subcircuits | Anschlüsse in der Reihenfolge von `ports` | `subcircuit`, `parameters` |
| `S`, `switch` | idealer Schalter | a, b [, Steuer+, Steuer−] | `control` (Ausdruck, bei zwei Knoten), `vt`, `vh`, `ron`, `roff`, `initial` |
| `ideal_diode` | ideale Diode | Anode, Kathode | `vf`, `ron`, `roff` |
| `D`, `diode` | Diode | Anode, Kathode | `is` `n` `rs` `cj0` `vj` `m` `fc` `tt` `bv` `ibv` `eg` `xti` `kf` `af` |
| `Q`, `npn`, `pnp` | Bipolartransistor | C, B, E | `is` `bf` `br` `nf` `nr` `vaf` `var` `ikf` `ikr` `ise` `ne` `isc` `nc` `rb` `rc` `re` `cje` `vje` `mje` `cjc` `vjc` `mjc` `tf` `tr` `fc` `eg` `xti` `xtb` `kf` `af` |
| `M`, `nmos`, `pmos` | MOSFET | D, G, S [, Bulk] | `vto` `kp` `gamma` `phi` `lambda` `w` `l` `tox` `cgso` `cgdo` `cgbo` `cbd` `cbs` `pb` `mj` `fc` `is` `tcv` `kf` `af`; mit `"level": "ekv"` zusätzlich `theta` `ucrit`; mit `"level": 3` zusätzlich `theta` `eta` `vmax` `nfs` `kappa` `xj` `ld` `delta` `u0` `nsub` `nss` `tpg`; mit `"level": "bsim3"` die Parameter von BSIM3v3 und `w` `l` `ad` `as` `pd` `ps` `m` |

Verhaltensquelle `B`:

- `"i": "..."` Strom vom Plus-Knoten durch das Bauteil zum Minus-Knoten. Ein
  nichtlinearer Widerstand ist `"i": "1e-3*tanh(v(a,b)/0.1)"`.
- `"v": "..."` Spannung zwischen den Knoten (mit eigenem Zweigstrom `i(Name)`).
- `"q": "..."` gespeicherte Ladung; der Strom ist ihre Zeitableitung. Ein
  spannungsabhängiger Kondensator ist `"q": "C0*v(a,b) + C1*v(a,b)^2"`.
- `"phi": "..."` magnetischer Fluss; die Spannung ist seine Zeitableitung. Der
  Ausdruck hängt in der Regel vom eigenen Strom `i(Name)` ab. Eine sättigende
  Drossel ist `"phi": "L0*Isat*tanh(i(L1)/Isat)"`.

Kopplung `K`: `{ "type": "K", "inductors": ["L1", "L2"], "k": 0.99 }` koppelt zwei
Spulen mit der Gegeninduktivität `M = k·sqrt(L1·L2)`. Der erste Knoten jeder
Spule trägt den Punkt. Ein Übertrager mit Übersetzung n hat `L2 = n²·L1`.

Schalter `S`:

- Mit vier Knoten steuert die Spannung zwischen den Steuerknoten (wie in SPICE),
  mit zwei Knoten ein beliebiger Ausdruck: `"control": "v(gate)"`,
  `"control": "i(L1)"`, `"control": "sin(2*pi*50*t)"`, eine Variable, …
- Der Schalter schließt, wenn die Steuergröße über `vt + vh` steigt, und öffnet,
  wenn sie unter `vt − vh` fällt (`vt` Schwelle, `vh` halbe Hysteresebreite,
  beide Standard 0).
- `ron` (Standard 1 mΩ) und `roff` (Standard 1 GΩ) sind die Widerstände in
  beiden Stellungen. `"initial": "on"` legt die Stellung fest, solange die
  Steuergröße beim Start innerhalb des Hysteresebands liegt.

Ideale Diode `ideal_diode`: sperrt unterhalb der Flussspannung `vf` (Standard 0)
mit `roff` und leitet darüber mit der Steigung `1/ron`. Im Unterschied zur
Diode `D` hat sie keine Exponentialkennlinie und keine Ladung; sie ist für
Leistungselektronik und Gleichrichter gedacht, wenn nur das Schaltverhalten
interessiert.

Beide Bauteile schalten als [Ereignis](#ereignisse): Der Umschaltzeitpunkt wird
exakt bestimmt. Hängen mehrere voneinander ab (ein Schalter öffnet an einer
Spule, also muss die Freilaufdiode leiten), stellen sie sich im selben
Zeitpunkt gemeinsam ein, ohne künstliche Spannungsspitze.

Halbleiterparameter können direkt am Bauteil stehen oder über `"model": "<Name>"`
aus `models` kommen; Angaben am Bauteil überschreiben das Modell (z. B. `w`, `l`).
Die Polarität eines Transistors ergibt sich aus dem `type` des Bauteils (`npn`,
`pmos`, …) oder des Modells. Die Parameternamen und Standardwerte folgen SPICE.

Ein Modell kann auch als SPICE-Zeile angegeben werden, wie sie Hersteller
veröffentlichen (hier ein allgemeines Modell mit Lehrbuchwerten):

```jsonc
"models": {
  "QGEN": ".model QGEN NPN(Is=1e-14 Bf=200 Br=2 Vaf=100 Ikf=0.3 Rb=10 Rc=1 Cje=10p Cjc=5p Tf=0.5n Tr=50n)"
}
```

Unterstützt werden die Typen `D`, `NPN`, `PNP`, `NMOS`, `PMOS`, `NJF`, `PJF`. Zahlen folgen
dabei der SPICE-Schreibweise (`m` = Milli, `meg` = Mega, ohne Unterscheidung
von Groß- und Kleinschreibung). Parameter, die hier keine Wirkung haben, werden
einmal aufgelistet und übergangen.

Modellumfang:

- **Diode**: Shockley-Gleichung mit Bahnwiderstand, Sperrschicht- und
  Diffusionskapazität, Durchbruch (`bv`, `ibv`) für Z-Dioden.
- **BJT**: Gummel-Poon-Modell mit Early-Effekt (`vaf`, `var`),
  Hochstromabfall der Verstärkung (`ikf`, `ikr`), Leckströmen (`ise`, `ne`,
  `isc`, `nc`), Bahnwiderständen (`rb`, `rc`, `re`) sowie Sperrschicht- und
  Diffusionsladungen. Nicht enthalten: Substratdiode, stromabhängiger
  Basiswiderstand, Arbeitspunktabhängigkeit der Transitzeit (`xtf`, `vtf`, `itf`).
- **JFET**: Shichman-Hodges-Modell (quadratische Kennlinie, Kanallängen-
  modulation) mit Gate-Sperrschichten und deren Kapazitäten.
- **MOSFET**: SPICE Level 1 (Shichman-Hodges) mit Substrateffekt,
  Kanallängenmodulation, Bulk-Dioden, Überlapp- und Sperrschichtkapazitäten.
  Mit `tox` (Oxiddicke) kommt die Ladung des Kanals hinzu: ladungserhaltend,
  in Sättigung 40 : 60 auf Drain und Source verteilt, Gate-Kapazität 2/3·Cox in
  Sättigung und Cox im Widerstandsbereich und unterhalb der Schwelle.
- **MOSFET mit `"level": "ekv"`**: ladungsbasiertes Modell nach
  Enz, Krummenacher und Vittoz. Ein einziger glatter Ausdruck beschreibt den
  Strom von schwacher bis starker Inversion: unterhalb der Schwelle fällt er
  exponentiell (n·Vt·ln 10 je Dekade, n aus `gamma`), weit darüber gilt das
  Quadratgesetz von Level 1. Dazu Beweglichkeitsreduktion durch das Gate-Feld
  (`theta` in 1/V), Geschwindigkeitssättigung (`ucrit` in V/m, wirkt mit der
  Kanallänge) und Kanallängenmodulation (`lambda`). Das Modell ist symmetrisch
  in Drain und Source, alle Ableitungen sind stetig, und die Ladungen (mit
  `tox`) stammen aus demselben Ansatz. Es ersetzt kein BSIM: Kurzkanaleffekte
  wie DIBL, Gate-Ströme oder die Parametersätze der Halbleiterhersteller gibt
  es nicht. Wo Level 1 unterhalb der Schwelle exakt 0 liefert (Leckströme,
  Schaltungen im Unterschwellbetrieb, Analogschalter nahe 0 V), ist es die
  bessere Wahl; `examples/mosfet_subthreshold.json` vergleicht beide.
- **MOSFET mit `"level": 3`** (SPICE MOS3): das halbempirische
  Kurzkanalmodell, das viele Herstellermodelle von MOSFETs benutzen (z. B.
  die von Transim erzeugten Leistungs-MOSFETs). Schwellspannung mit
  Substrat-, Kurzkanal- (`xj`, `ld`, mit `nsub`) und Schmalkanaleffekt
  (`delta`) und statischer Rückwirkung (`eta`), Beweglichkeit mit
  Gate-Feld (`theta`), Geschwindigkeitssättigung (`vmax`),
  Kanallängenmodulation (`kappa`, mit `nsub`) und Strom unterhalb der
  Schwelle (`nfs`). Einheiten wie in SPICE (`u0` in cm²/Vs, `nsub` in 1/cm³,
  `nfs` in 1/cm²); ohne `kp` folgt es aus `u0` und `tox`, ohne `tox` gilt
  1e-7 m; aus `nsub` folgen fehlende `gamma`, `phi` und `vto`.
  Temperaturgang wie ngspice. Der Drainstrom stimmt mit ngspice (MOS3) auf
  besser als 1e-9 überein (`tests/mos3_ngspice.py`); die Gate-Ladung ist
  die ladungserhaltende von Level 1 mit der Schwellspannung von Level 3
  (ngspice: Kapazitäten nach Meyer), was Schaltflanken leicht verändert.
- **MOSFET mit `"level": "bsim3"`** (SPICE-Level 8, 49 und 53): der
  Drainstrom nach BSIM3v3.3, dem Kurzkanalmodell, für das Halbleiterhersteller
  Parametersätze veröffentlichen. Umgesetzt sind Schwellspannung mit Kurz- und
  Schmalkanaleffekten und DIBL, Unterschwellstrom, die drei
  Beweglichkeitsmodelle, Geschwindigkeitssättigung, Bahnwiderstände,
  Kanallängenmodulation, DIBL und SCBE im Ausgangsleitwert, Poly-Verarmung,
  Substratstrom, Temperaturabhängigkeit und die Größenabhängigkeit aller
  Parameter über ihre L-, W- und P-Varianten. Die Ladungen folgen den
  Kapazitätsmodellen von BSIM3 (`capmod` 0 bis 3, Standard 3): 0 stückweise
  mit eigener Flachbandspannung `vfbcv`, 1 und 2 mit `vfbzb` aus der
  Schwellspannung (2 mit glatten Übergängen), 3 das Charge-Thickness-Modell,
  bei dem Inversions- und Akkumulationsschicht einen Abstand vom Oxid haben
  (`acde`, `moin`). Die Aufteilung der Kanalladung auf Drain und Source wählt
  `xpart` (0: 40/60, 0,5: 50/50, 1: 0/100, negativ: keine Kanalladung). Dazu
  `noff`, `voffcv`, `clc`, `cle`, `dlc`, `dwc` mit `llc` … `wwlc` und die
  spannungsabhängigen Überlappkapazitäten `cgsl`, `cgdl`, `ckappa` (ab
  `capmod` 1). Es gelten die Gleichungen ab BSIM3v3.2: capMod 1 und 2 nehmen
  `vfbzb`, auch wenn die Karte `VERSION=3.1` angibt. Modellkarten lassen sich als
  SPICE-Zeile einfügen; lange Karten als Liste ihrer Zeilen:

  ```jsonc
  "models": { "n18": [".model n18 NMOS (LEVEL=49 TOX=4.1E-9 VTH0=0.369",
                      "+ K1=0.579 U0=293 VSAT=1.68E5 ...)"] }
  ```

  Geprüft sind Drainstrom und Ladungen gegen eine zweite, getrennt
  geschriebene Umsetzung derselben Modellgleichungen in Python
  (`tests/bsim3_reference.py`): beim Drainstrom 16 Parametersätze (n- und
  p-Kanal, −40 bis 125 °C, alle Zweige des Modells) mit je 720 Arbeitspunkten,
  Übereinstimmung auf 5·10⁻⁹; bei den Klemmenladungen 25 Parametersätze (alle
  `capmod` und `xpart`, Überlapp, Vertauschung von Drain und Source) mit je 630
  Arbeitspunkten, Übereinstimmung auf 10⁻⁸. Mit denselben Fällen vergleicht
  `tests/bsim3_ngspice.py` gegen BSIM3v3.3 in ngspice (Level 8, Version
  3.3.0): Drainstrom und Klemmenladungen stimmen auf 10⁻⁹ überein. Der Test
  läuft mit `ctest`, wenn ngspice installiert ist; die Sperrschichtladungen
  vergleicht er nicht.

  Sperrschichtkapazitäten (`cj`, `cjsw`, `cjswg` mit `ad`, `as`, `pd`, `ps`)
  sind enthalten. Es fehlen die BSIM3-Rauschmodelle (es gelten die Formeln von
  Level 1), nicht-quasistatische Effekte (`nqsmod`, `elm`) und die Auswahl
  zwischen mehreren Modellkarten nach `lmin`/`lmax`.
  Parameter, die deshalb ohne Wirkung bleiben, werden still akzeptiert;
  unbekannte werden gemeldet. Beispiel: `examples/ring_oscillator_bsim3.json`.
- **Leitung** (`T`): ideale, verlustlose Leitung mit Wellenwiderstand `z0` und
  Laufzeit `td`. In der Zeitsimulation wird die Vorgeschichte der Wellen an
  beiden Toren gespeichert; kein Schritt ist länger als `td`, und Flanken
  setzen an ihrer Ankunftszeit einen Haltepunkt. Im Arbeitspunkt ist die
  Leitung eine Verbindung, in der AC-Analyse wirkt die Laufzeit exakt als
  Phasendrehung. `i(T1)` ist der Strom in Tor 1.
- **Verlustbehaftete Leitung**: Mit dem Längswiderstand `r` und dem
  Querleitwert `g` (je Längeneinheit, Länge `len`, Standard 1) wird die Leitung
  in verlustlose Abschnitte geteilt, zwischen denen die Verluste als
  Widerstände sitzen. Die Laufzeit bleibt exakt; der Fehler der Aufteilung
  fällt mit dem Quadrat der Abschnittszahl. Ohne Angabe von `segments` wird sie
  so gewählt, dass auf jeden Abschnitt höchstens ein Zehntel von `z0` entfällt
  (im Test weicht die AC-Antwort dann um weniger als 0,5 % von der exakten
  Lösung der Leitungsgleichungen ab). Jeder Abschnitt begrenzt den Zeitschritt
  auf seine Laufzeit. Der Skineffekt (Verluste, die mit der Frequenz wachsen)
  ist nicht modelliert. `i(T1)` ist hier der Strom in den ersten Abschnitt,
  ohne den Anteil des ersten Querleitwerts.
- **Temperatur**: `options.temp` ist die Temperatur der Schaltung, `tnom`
  (Standard 27 °C) die, für die die Modellparameter gelten. Skaliert werden die
  Sättigungsströme aller pn-Übergänge (`eg`, `xti`), die Stromverstärkung
  (`xtb`), die Beweglichkeit im MOSFET (`kp` ~ T^−1,5) und auf Wunsch die
  Schwellspannung (`tcv` in V/K). Sperrschichtpotentiale und Kapazitäten
  bleiben unverändert.

### Subcircuits

Wiederverwendbare Teilschaltungen stehen unter `subcircuits` und werden mit dem
Bauteil `X` eingesetzt:

```jsonc
"subcircuits": {
  "divider": {
    "ports": ["in", "out"],
    "parameters": { "ratio": 0.5, "rtotal": "10k" },
    "components": [
      { "type": "R", "name": "R1", "nodes": ["in", "out"], "value": "(1 - ratio)*rtotal" },
      { "type": "R", "name": "R2", "nodes": ["out", "0"],  "value": "ratio*rtotal" }
    ]
  }
},
"components": [
  { "type": "X", "name": "X1", "subcircuit": "divider", "nodes": ["a", "b"] },
  { "type": "X", "name": "X2", "subcircuit": "divider", "nodes": ["a", "c"],
    "parameters": { "ratio": 0.25 } }
]
```

- **Knoten:** Die Namen in `ports` stehen für die außen angeschlossenen Knoten,
  alle anderen Knoten sind lokal. Masse (`"0"`) ist überall dieselbe.
- **Parameter:** `parameters` enthält die Standardwerte; sie dürfen sich auf
  globale Parameter und aufeinander beziehen. Die Instanz überschreibt einzelne
  Werte mit `"parameters": {...}`.
- **Inhalt:** alle Bauteile, auch weitere Instanzen (Schachtelung bis 20 Ebenen),
  dazu eigene `variables`, `equations` und `events`. Ein Bauteil mit
  thermischem Zustand oder eigener Kennlinie lässt sich so als Modell kapseln
  und mehrfach verwenden.
- **Namen von außen:** Was in der Instanz `X1` liegt, heißt `X1.<Name>`, bei
  Schachtelung `X1.U.<Name>`: `v(X1.mid)`, `i(X1.V1)`, die Variable `X1.T`,
  die Quelle `"source": "X1.V1"` im DC-Sweep. In der Standardausgabe (ohne
  `outputs`) erscheint das Innere von Instanzen nicht.
- Innerhalb eines Subcircuits beziehen sich `i(...)` und `control` auf Bauteile
  desselben Subcircuits.

**Subcircuits in SPICE.** Modelle der Hersteller lassen sich unverändert
einsetzen: Statt eines Objekts steht der SPICE-Text, als ein String oder eine
Liste von Zeilen.

```jsonc
"subcircuits": {
  "OPX": [
    ".SUBCKT OPX inp inn out vp vn",
    "Rin inp inn 1meg",
    "Egain mid 0 POLY(1) inp inn 0 1e5",
    "Rout mid out 10",
    "Dhi out vp DCLAMP",
    "Dlo vn out DCLAMP",
    ".MODEL DCLAMP D(IS=1e-14)",
    ".ENDS"
  ]
},
"components": [
  { "type": "X", "name": "U1", "subcircuit": "OPX", "nodes": ["in", "fb", "out", "vp", "vn"] }
]
```

- Gelesen werden R, C, L, K, V und I (DC, AC, SIN, PULSE, PWL), E und G
  (linear, `POLY(n)`, `VALUE = {…}`, `TABLE {…} = (x, y) …`), F und H (linear,
  `POLY(n)`), B (`V=` oder `I=`), D, Q, M, J, S, T und X; dazu `.param`,
  `PARAMS:` an `.subckt` und an Instanzen, `.model` (auch mit Ausdrücken
  wie `KF={PWR(FLW,0.5)/1E11}`, ausgewertet mit den Parametern des
  Subcircuits).
- Formen von PSpice: Widerstände mit Modell (`R1 a b RMOD 1k`; `R=` im
  Modell skaliert, `T_ABS=-273.15` macht den Widerstand rauschfrei, als
  `"noise": false`); Schalter mit `VSWITCH(RON ROFF VON VOFF)` mit dem
  stetigen Übergang von PSpice zwischen VOFF und VON (als Verhaltensquelle);
  Schalter mit `SW(VT VH)` werden esims Schalter mit Hysterese.
- Erprobt mit dem MOSFET PMZB950UPEL von Nexperia (Level 3): Kennlinien
  wie ngspice auf 6 Stellen, Schaltflanke auf 17 mV bei 5 V Hub.
- Erprobt mit dem Makromodell des LM358/LM2904 von TI (PSpice, 18
  Teilschaltungen, 227 Bauteile): DC-Kennlinie und Sprungantwort stimmen
  mit ngspice im PSpice-Modus überein (DC auf 7 Stellen, Zeitverlauf auf
  0,3 mV).
- Ausdrücke in `{…}` werden übersetzt: Zahlen mit SPICE-Endungen (`M` ist
  milli, `meg` mega), `**`, `a ? b : c`, `limit`, `pwr`, `pwrs`, `uramp`,
  `u`, `stp`, `sgn`.
- Modelle und weitere `.subckt` im Text gehören zu diesem Subcircuit und
  heißen intern `OPX/NAME`; sie stoßen nicht mit Modellen der Datei
  zusammen. Was der Text nicht definiert, sucht esim in der Datei.
- Nicht Unterstütztes (z. B. `LAPLACE`, `.include`, unbekannte Bauteile)
  ist ein Fehler, der die Zeile im Text nennt. Ohne Wirkung bleiben, mit
  Warnung, z. B. die Fläche von Dioden und der Substratknoten von
  Transistoren.
- `tests/spice_subckt_ngspice.py` vergleicht eine solche Teilschaltung mit
  ngspice.

### Quellen

`dc` ist der Wert für Arbeitspunkt, DC-Sweep und AC-Arbeitspunkt. `wave`
beschreibt den Zeitverlauf für die Transientenanalyse; ohne `dc` gilt in den
DC-Analysen der Wert von `wave` bei t = 0. `ac` (Amplitude) und `acphase`
(Grad) sind die Kleinsignalanregung für die AC-Analyse.

| wave.type | Parameter |
|-----------|-----------|
| `sin` | `offset`, `amplitude`, `freq`, `delay`, `damping` (1/s), `phase` (Grad) |
| `pulse` | `v1`, `v2`, `delay`, `rise`, `fall`, `width`, `period` (ohne `period`: Einzelpuls; `rise`/`fall` Standard 0,1 % der Periode) |
| `pwl` | `points`: `[[t0, v0], [t1, v1], …]` |
| `dc` | `value` |

Vorzeichen: `i(V1)` ist der Strom vom Plus-Knoten *durch die Quelle* zum
Minus-Knoten, also negativ, solange die Quelle Leistung abgibt.

### Analysen

| type | Parameter | Ergebnis |
|------|-----------|----------|
| `op` | – | Arbeitspunkt (Konsole) |
| `dc` | `source`, `start`, `stop`, `step` | Sweep einer V- oder I-Quelle |
| `ac` | `sweep` (`dec`/`oct`/`lin`), `points`, `fstart`, `fstop` | Betrag und Phase (Grad) je Ausgang |
| `tran` | `tstop`, optional `tstep`, `hmax`, `hinit`, `hmin`, `method`, `max_order`, `uic`, `output_step`, `max_events`, `fourier` | Zeitverlauf |
| `noise` | `output`, optional `input`, dazu `sweep`, `points`, `fstart`, `fstop` wie bei `ac` | Rauschdichte am Ausgang und auf den Eingang bezogen |
| `pss` | `period`, optional `periods`, `tstab`, `tol`, `max_iterations` und die Angaben von `tran` | eingeschwungener periodischer Verlauf |
| `sens` | `analysis`, optional `parameters`, `delta` | Empfindlichkeiten gegenüber Parametern |
| `fit` | `analysis`, `output`, `data`, `parameters` | an Messdaten angepasste Parameter |

Jede Analyse kann mit `"outputs": [...]` eigene Ausgänge und mit `"name"` einen
eigenen Dateinamen bekommen.

Transientenanalyse:

- Start: Die Schaltung beginnt in ihrem Arbeitspunkt, Zustände von Gleichungen
  bei ihren Anfangswerten (siehe oben).
- `"uic": true` überspringt diese Anfangslösung und startet von den
  Anfangsbedingungen: `ic` an Kondensatoren und Spulen,
  `"ic": {"knoten": spannung}` auf oberster Ebene, die Startwerte der
  Variablen. Knoten, die Spannungsquellen von Masse aus festlegen
  (Versorgungen, auch über Strommesser hinweg), starten auf deren Spannung;
  alles andere ist 0.
- Die Schrittweite wird automatisch gewählt. `tstep` ist die gewünschte
  Auflösung: kein Schritt wird länger (Standard: `tstop/50`).
- `method` wählt das Verfahren:
  - `bdf` (Standard): BDF mit variabler Ordnung 1 bis 5. Nach jedem Schritt
    wird die Ordnung gewählt, die den größten nächsten Schritt erlaubt. Bei
    glatten Lösungen und engen Toleranzen braucht es ein Vielfaches weniger
    Schritte als ein Verfahren fester Ordnung; `"max_order"` begrenzt die Ordnung.
  - `gear2`: BDF fester Ordnung 2. Robust, klingelt nicht, dämpft
    Schwingungen leicht.
  - `trap`: Trapezregel. Keine numerische Dämpfung, deshalb die beste Wahl
    für schwach gedämpfte Schwingkreise über viele Perioden; dafür können
    Ströme von Spannungsquellen von Schritt zu Schritt leicht alternieren.
  - `be`: Rückwärts-Euler, erste Ordnung, sehr robust und stark dämpfend.
- Sinusquellen begrenzen die Schrittweite auf ein Zwanzigstel ihrer Periode.
- `"output_step": dt` gibt die Ergebnisse auf dem festen Raster 0, dt, 2·dt, …
  aus (interpoliert zwischen den Zeitpunkten des Lösers, die Schrittweite
  bleibt frei). Ohne diese Angabe wird jeder Zeitpunkt des Lösers ausgegeben.
- `"max_events"` (Standard 100000) begrenzt die Zahl der Ereignisse.

### Periodischer stationärer Zustand

Schaltnetzteile, Gleichrichter oder Verstärker mit großen Koppelkondensatoren
brauchen oft Tausende von Perioden, bis sie eingeschwungen sind. Die Analyse
`pss` sucht den eingeschwungenen Zustand direkt:

```jsonc
{ "type": "pss", "period": "10u",
  "measure": { "vout": { "avg": "v(out)" }, "ripple": { "pp": "v(out)" } } }
```

- `period` ist die Periode der Anregung. Alle Quellen müssen sich mit ihr
  wiederholen; das wird geprüft.
- Gesucht wird ein Zustand, der nach einer Periode wieder erreicht ist. Jede
  Iteration rechnet eine Periode und schätzt aus den letzten Ergebnissen, wohin
  die langsamen Vorgänge laufen (Anderson-Beschleunigung). Der Abwärtswandler
  aus `examples/buck_converter.json` braucht so 5 Perioden statt etwa 2000.
- Ergebnis ist eine Periode im eingeschwungenen Zustand (`"periods": n` für
  mehrere); Messgrößen, `fourier` und `output_step` wirken wie bei `tran`.
- `tol` (Standard 1e-4) ist die relative Abweichung, mit der sich der Zustand
  nach einer Periode wiederholen muss. `tstab` rechnet vor der Suche eine Zeit
  lang normal, was bei stark nichtlinearem Anlauf hilft.
- Start wie bei `tran` (Arbeitspunkt oder `"uic": true`).

Die Periode muss bekannt sein. Für selbstschwingende Schaltungen (Oszillatoren)
ist die Analyse nicht gedacht, ebenso wenig für Zustände, die erst nach einem
Schaltvorgang mit Gedächtnis erreicht werden (Hysterese, diskrete Variablen):
Solche Größen werden von der Extrapolation nicht erfasst.

### Empfindlichkeiten

Die Analyse `sens` beantwortet die Frage, welche Parameter ein Ergebnis
bestimmen. Sie ändert jeden Parameter um ±`delta` (relativ, Standard 1e-3),
wiederholt die eingebettete Analyse und bildet den Differenzenquotienten:

```jsonc
{ "type": "sens", "parameters": ["R1", "R2", "C"],
  "analysis": { "type": "ac", "sweep": "dec", "points": 100, "fstart": 1, "fstop": "100k",
                "measure": { "fc": { "f3db": "v(out)" } } } }
```

- Betrachtet werden die Messgrößen der Analyse, bei `op` ihre Ausgänge.
- Ohne `parameters` werden alle Einträge von `parameters` variiert.
- Ausgegeben wird die normierte Empfindlichkeit: um wie viel Prozent sich das
  Ergebnis ändert, wenn der Parameter um ein Prozent wächst. Die Tabelle steht
  in `<Name>.sens.csv`; die Zusammenfassung (`--summary`) enthält zusätzlich
  die absoluten Ableitungen.
- Messgrößen, die aus abgetasteten Kurven stammen (Grenzfrequenzen,
  Durchgangszeiten), sind nur so genau wie das Raster. Dann ein größeres
  `delta` (z. B. 0.02) oder ein feineres Raster wählen.

Der Aufwand sind zwei Rechnungen je Parameter.

### Fourier-Analyse

Der Block `fourier` einer Transientenanalyse berechnet das Spektrum der letzten
ganzen Periode(n) zur Grundfrequenz `freq`:

```jsonc
{ "type": "tran", "tstop": "5m", "tstep": "10u",
  "fourier": { "freq": "1k", "harmonics": 9, "outputs": ["v(out)"] } }
```

Das Ergebnis steht in `<Name>.tran.fourier.csv` (Betrag und Phase je
Harmonischer, Zeile 0 ist der Mittelwert); Grundschwingung und Klirrfaktor
erscheinen auf der Konsole. `periods` (Standard 1) wählt die Zahl der
ausgewerteten Perioden, `from` den frühesten Zeitpunkt. Die Genauigkeit hängt
von der Auflösung des Zeitverlaufs ab: für kleine Klirrfaktoren `tstep` und
`reltol` klein wählen. Als einzelne Kennwerte gibt es die Messgrößen `thd` und
`harmonic` (siehe unten).

### Rauschanalyse

```jsonc
{ "type": "noise", "output": "v(out)", "input": "Vin",
  "sweep": "dec", "points": 20, "fstart": 10, "fstop": "100k" }
```

Berechnet im Arbeitspunkt die spektrale Rauschdichte am Ausgang (`onoise`, bei
einer Spannung in V/√Hz) und, wenn eine Quelle als `input` genannt ist, die
auf sie bezogene Dichte (`inoise`) samt Verstärkung. Auf der Konsole stehen das
über das Band integrierte Rauschen und die größten Beiträge. `output` ist
`v(knoten)`, `v(a,b)` oder `i(bauteil)`.

Rauschquellen: thermisches Rauschen aller Widerstände (auch der Bahnwiderstände
von Diode und Transistor), Schrotrauschen von Dioden-, Kollektor- und
Basisstrom, thermisches Kanalrauschen des MOSFET, dazu 1/f-Rauschen über die
Modellparameter `kf` und `af`. Schalter, ideale Dioden und Verhaltensquellen
rauschen nicht.

### Messgrößen

Mit `measure` berechnet eine Analyse Kennwerte aus ihren Kurven. Ohne Sweep
erscheinen sie auf der Konsole:

```jsonc
{ "type": "tran", "tstop": "200m",
  "measure": {
    "vdc":    { "avg": "v(out)", "from": "160m" },
    "ripple": { "pp":  "v(out)", "from": "160m" },
    "t90":    { "cross": "v(out)", "level": 13.5 }
  } }
```

Jede Messung besteht aus einer Funktion mit dem auszuwertenden Ausdruck und
Optionen. `x` ist dabei die Zeit, die Frequenz oder der Wert der gesweepten
Quelle; in der AC-Analyse wird der Betrag ausgewertet.

| Funktion | Ergebnis | Optionen |
|----------|----------|----------|
| `min`, `max`, `pp` | Extremwerte, Spitze-Spitze | |
| `argmin`, `argmax` | x, bei dem der Extremwert liegt | |
| `avg`, `rms`, `integ` | Mittelwert, Effektivwert, Integral über x | |
| `final` | letzter Wert | |
| `at` | Wert an der Stelle `x` | `x` |
| `cross` | x des `n`-ten Durchgangs durch `level` | `level`, `n` (Standard 1), `edge`: `rise`, `fall`, `both` |
| `period`, `freq`, `duty` | Periode, Frequenz, Tastgrad aus den steigenden Durchgängen | `level` (Standard: Mitte zwischen Minimum und Maximum) |
| `rise`, `fall` | Anstiegs- / Abfallzeit zwischen zwei Pegeln | `low`, `high` (Standard: 10 % und 90 % des Hubs) |
| `f3db`, `f3db_low` | obere / untere Stelle, an der die Kurve auf Maximum/√2 gefallen ist | |
| `thd`, `harmonic` | Klirrfaktor (bis zur 15. Harmonischen) bzw. Amplitude der `n`-ten Harmonischen | `fundamental` (Grundfrequenz), `n` |

`from` und `to` beschränken jede Messung auf einen Ausschnitt. Eine Messung
ohne Ergebnis (z. B. kein Durchgang gefunden) liefert „not defined" bzw. `nan`.
Ausgewertet werden die ausgegebenen Punkte, mit `output_step` also das Raster.

In einem Parameter-Sweep entsteht aus den Messungen eine Tabelle
`<Name>.<Analyse>.measure.csv` mit einer Zeile je Parameterwert, etwa
„Periode über C". `"save": false` an der Analyse unterdrückt die Datei mit den
Kurven, wenn nur die Kennwerte interessieren.

### Parameter-Sweep

Jede Analyse lässt sich für mehrere Werte eines Parameters wiederholen. Alle
Läufe stehen in einer CSV-Datei, deren erste Spalte der Parameterwert ist:

```jsonc
"parameters": { "C": "100n" },
"analyses": [
  { "type": "tran", "tstop": "8m",
    "param_sweep": { "param": "C", "values": ["47n", "100n", "220n"] } },
  { "type": "ac", "fstart": 10, "fstop": "1meg",
    "param_sweep": { "param": "C", "start": "1n", "stop": "1u", "points": 4, "scale": "log" } }
]
```

`scale` ist `lin` (Standard) oder `log`. Mit `op` entsteht eine Tabelle mit
einer Zeile je Parameterwert. Abhängige Parameter (`"tau": "R*C"`) und alle
Bauteilwerte, die den Parameter verwenden, werden für jeden Wert neu berechnet.

Eine Liste von Sweeps wird verschachtelt (der erste ist der äußere); die
Tabellen bekommen dann eine Spalte je Parameter:

```jsonc
"param_sweep": [ { "param": "R", "values": ["9.9k", "10k", "10.1k"] },
                 { "param": "C", "start": "9n", "stop": "11n", "points": 3 } ]
```

### Monte Carlo

Mit `monte_carlo` wird eine Analyse mit zufällig gestreuten Parametern
wiederholt. Ausgewertet werden ihre Messgrößen:

```jsonc
{ "type": "ac", "fstart": 1, "fstop": "100k", "points": 100,
  "monte_carlo": { "runs": 500, "seed": 1,
                   "vary": { "R": { "tol": 0.01 },
                             "C": { "tol": 0.10, "dist": "gauss" },
                             "Voff": { "sigma": "2m" } } },
  "measure": { "fc": { "f3db": "v(out)" } } }
```

- `tol` ist eine relative Toleranz: gleichverteilt in ±tol, mit
  `"dist": "gauss"` normalverteilt mit Standardabweichung tol/3.
- `sigma` ist eine absolute Standardabweichung (normalverteilt).
- Auf der Konsole stehen Mittelwert, Standardabweichung, Minimum und Maximum
  jeder Messgröße; `<Name>.<Analyse>.measure.csv` enthält je Lauf die gezogenen
  Parameter und die Messwerte. Derselbe `seed` liefert dieselben Läufe.
- Die Kurven aller Läufe werden nur mit `"save": true` gespeichert.

### Anpassung an Messdaten

Die Analyse `fit` verändert Parameter so, dass eine simulierte Kurve möglichst
gut auf Datenpunkten liegt (kleinste Quadrate, Levenberg-Marquardt):

```jsonc
{ "type": "fit",
  "analysis": { "type": "dc", "source": "V1", "start": 0.25, "stop": 0.75, "step": 0.005 },
  "output": "-i(V1)",
  "data": { "file": "data/diode_iv.csv", "xcol": 0, "ycol": 1 },
  "parameters": { "is0": { "min": 1e-14, "max": 1e-6, "log": true },
                  "n0":  { "min": 1, "max": 3 } },
  "residual": "log" }
```

- `analysis` ist eine `dc`-, `ac`- oder `tran`-Analyse; `output` die Größe, die
  mit den Daten verglichen wird (bei `ac` ihr Betrag). Die Daten müssen
  innerhalb des simulierten Bereichs liegen; dazwischen wird linear interpoliert.
- `data`: `x` und `y`, `points` oder eine Datei wie bei den Tabellen.
- `parameters`: je Parameter optional `start` (sonst der Wert aus
  `parameters`), `min`, `max` und `"log": true`, wenn sich der Wert über
  Größenordnungen bewegen kann.
- `residual`: `lin` (Differenz, Standard), `rel` (relativ zu den Daten) oder
  `log` (Logarithmus des Verhältnisses, für Daten über mehrere Dekaden).
- Ergebnis: die angepassten Werte und der mittlere Restfehler auf der Konsole,
  Daten und Modellkurve in `<Name>.fit.csv`.

Das Verfahren findet ein lokales Minimum; die Startwerte sollten in der
richtigen Größenordnung liegen. Parameter, die die Kurve im gemessenen Bereich
kaum beeinflussen, bleiben unbestimmt.

### Ausgänge

`"outputs"` enthält `v(knoten)`, `v(a,b)`, `i(bauteil)`, Variablennamen oder
beliebige Ausdrücke daraus, z. B. `"v(out)*i(V1)"` oder `"T - 273.15"`. Die
AC-Analyse akzeptiert nur die einfachen Formen. Zweigströme gibt es für
Spannungsquellen, Spulen, E-, H- und B-Spannungsquellen; für andere Ströme eine
0-V-Quelle in Reihe schalten. Ohne `outputs` werden alle Knotenspannungen,
Zweigströme und Variablen ausgegeben.

### Optionen und Startwerte

```jsonc
"options": {
  "reltol": 1e-4,       // relative Toleranz: Newton und Fehler pro Zeitschritt
  "vntol": 1e-6,        // absolute Toleranz für Spannungen [V]
  "abstol": 1e-12,      // absolute Toleranz für Ströme [A]
  "vartol": 1e-9,       // absolute Toleranz für Variablen (einzeln: "abstol" an der Variable)
  "lte_floor": 0.1,     // Bezugsgröße des Zeitschrittfehlers, siehe unten
  "trtol": 1,           // Faktor auf den zulässigen Zeitschrittfehler (>1: gröber, schneller)
  "gmin": 1e-12,        // Leitwert parallel zu jedem pn-Übergang [S]
  "temp": 27,           // Temperatur der Schaltung [°C]
  "tnom": 27,           // Temperatur, für die die Modellparameter gelten [°C]
  "maxiter_dc": 100,    // Newton-Iterationen im Arbeitspunkt
  "maxiter_tran": 20,   // Newton-Iterationen pro Zeitschritt
  "reuse_jacobian": true,   // faktorisierte Matrix innerhalb eines Zeitschritts weiterverwenden
  "index_reduction": true   // Systeme mit Index über 2 automatisch umformen
},
"nodeset": { "out": 2.5 },   // Startwerte für den Arbeitspunkt (z. B. bistabile Schaltungen)
"ic": { "c1": 5 }            // Knotenspannungen bei t = 0 für "uic"
```

Genauigkeit der Zeitintegration: Pro Schritt darf der Fehler einer Unbekannten
`reltol · max(|x|, lte_floor · Spitzenwert von |x|) + absolute Toleranz` betragen.

- `lte_floor: 0` misst den Fehler relativ zum momentanen Wert. Das ist die
  übliche Definition und richtig für Lösungen, die über Größenordnungen
  abklingen; bei schwingenden Signalen kostet jeder Nulldurchgang kleine Schritte.
- `lte_floor: 1` misst relativ zum bisherigen Spitzenwert: am schnellsten, für
  Signale mit gleichbleibender Amplitude ausreichend.
- Der Standard 0.1 liegt dazwischen.

Das ist eine Schranke für den Fehler *pro Schritt*. Über viele Schritte
summiert er sich; für genauere Ergebnisse `reltol` verkleinern. Mit dem
Standardverfahren `bdf` kostet das wenig, weil es dann höhere Ordnungen nutzt.

## Aufbau

```
src/core/      generischer DAE-Kern (hängt nur von Eigen ab)   -> Bibliothek esimcore
  DaeSystem.hpp    Schnittstelle: d/dt q(x) + f(x,t) = 0
  Newton           Newton-Verfahren für f + a*q + b = 0
  DcSolver         stationäre Lösung, mit gmin- und Quellen-Stepping als Rückfall
  Transient        BDF variabler Ordnung, BDF2, Trapez, Rückwärts-Euler;
                   Schrittweiten- und Ordnungssteuerung, Ereignissuche
  LinearSolver     dünnbesetzte LU-Zerlegung
  StructuralIndex  Indexbestimmung nach Pantelides
  History.hpp      Vorgeschichte für Totzeiten und Leitungen
src/expr/      Ausdrücke: Parser, Auswertung, automatisches Differenzieren,
               Zerlegung von Gleichungen in q und f               (in esimcore)
src/circuit/   das simulierte System als DaeSystem               -> Bibliothek esimcircuit
  Circuit          Knoten, Unbekannte, Stempeln aller Bauteile
  Device.hpp       Basisklasse und StampContext
  Symbols.hpp      Namen in Ausdrücken -> Unbekannte der Schaltung
  IndexReduction   Ableiten von Zwangsbedingungen bei Index über 2
  devices/         R, C, L, Kopplung, Quellen, Diode, BJT, MOSFET, Bsim3, JFET,
                   TransmissionLine, Equations (Gleichungsblock),
                   Behavioral (B-Quelle), Switching (Schalter, ideale Diode)
  Analysis         Arbeitspunkt, AC-Analyse
src/io/        JSON-Eingabe, SI-Zahlen, Ausgänge, CSV
src/app/       Ablauf der Analysen aus der Eingabedatei, Messgrößen
examples/      Eingabedateien und ein DAE in C++ (examples/dae)
tools/         plot.py zum Zeichnen der Ergebnisse, spice2json.py für SPICE-Netzlisten
python/        esim.py: Aufruf aus Python
editor/        esim-editor: Oberfläche (Qt 6), prüft mit esimcircuit, rechnet mit esim
tests/         Testsuite gegen analytische Lösungen
```

Numerik in Kürze:

- **Arbeitspunkt**: Newton mit SPICE-Spannungsbegrenzung an pn-Übergängen
  (`pnjlim`). Scheitert er, folgen nacheinander eine Pseudo-Transiente (jeder
  Knoten ist gedämpft an seine vorherige Lösung gebunden, die Dämpfung wird
  schrittweise gelöst; das folgt dem Einschaltvorgang und bewältigt auch lange
  Ketten von Stufen hoher Verstärkung), gmin-Stepping und Quellen-Stepping.
- **Lineare Algebra**: dünnbesetzte LU-Zerlegung. Die Struktur der Matrix wird
  einmal analysiert; die Bauteile melden ihre Einträge in fester Reihenfolge,
  sodass sie über eine gespeicherte Zuordnung direkt an ihren Platz
  geschrieben werden. Ist die Matrix gegenüber dem letzten Schritt unverändert
  (lineare Schaltung bei gleicher Schrittweite), entfällt die Zerlegung ganz.
  Bei nichtlinearen Schaltungen wird die zerlegte Matrix der ersten
  Newton-Iteration eines Zeitschritts für die folgenden weiterverwendet,
  solange jede Iteration die Korrektur mindestens halbiert und kein Bauteil
  seine Spannungen begrenzt (modifiziertes Newton-Verfahren). Das halbiert die
  Zahl der Zerlegungen bei praktisch gleicher Zahl von Iterationen und spart
  in den Testschaltungen rund 20 % Rechenzeit; `"reuse_jacobian": false`
  schaltet es ab.
- **Parallele Auswertung**: Große Schaltungen werden in 16 Blöcke
  aufeinanderfolgender Bauteile geteilt, die sich die Threads der Reihe nach
  holen. Jeder Block schreibt seine Matrixeinträge direkt in seinen festen
  Abschnitt der gemeinsamen Liste und seine Ströme und Ladungen in einen
  eigenen Puffer; die Puffer werden in fester Reihenfolge addiert. Die Blöcke
  sind für jede Thread-Zahl dieselben, deshalb sind die Ergebnisse bitgleich.
- **Anfangspunkt einer Transientenanalyse**: dasselbe Problem, wobei für jeden
  Zustand mit vorgegebenem Anfangswert eine Differentialgleichung durch
  "Zustand = Anfangswert" ersetzt wird. Algebraische Unbekannte ergeben sich
  daraus konsistent.
- **Transient**: implizite Integration, pro Schritt ein Newton-Problem
  `f(x) + a·q(x) + b = 0`. Die Schrittweite folgt aus dem Abbruchfehler der
  Ladungen `q` (Differenz zur Extrapolation ihres Verlaufs), nicht der
  Unbekannten selbst: So bleiben algebraische Größen außen vor, und Knoten,
  deren Potential gegen Masse nur schwach bestimmt ist (potentialfreie
  Schaltungsteile), stören die Schrittweitensteuerung nicht. Knicke der
  Quellen (Pulsflanken, PWL-Stützstellen) werden als Breakpoints exakt
  getroffen; danach startet die Integration neu mit Ordnung 1. Auch der erste
  Schritt nach einem Neustart ist fehlerkontrolliert: Als Vergleich dient die
  Tangente, deren Steigung das System selbst liefert (`dq/dt = −f`).
- **Ordnung** (Verfahren `bdf`): Aus dem Verlauf der Ladungen wird geschätzt,
  welchen Fehler derselbe Schritt mit der nächstniedrigeren und der
  nächsthöheren Ordnung gehabt hätte; gewählt wird die Ordnung mit dem größten
  zulässigen Schritt. Die Schrittweite bleibt konstant, bis sie verdoppelt
  werden kann oder verkleinert werden muss (wie in DASSL), weil Formeln hoher
  Ordnung ein stetig wachsendes Raster nicht vertragen.
- **Ereignisse**: Jeder Vergleich in einem Ausdruck liefert eine
  Ereignisfunktion. Wechselt eine davon in einem Schritt das Vorzeichen, wird
  der Schritt so lange verkürzt (Sekantenverfahren mit Bisektion als
  Absicherung, jeder Versuch ein vollständiger Schritt), bis er knapp hinter
  dem Vorzeichenwechsel endet. Zwischen zwei Ereignissen sind die Ausgänge der
  Vergleiche und die Stellungen der Schalter eingefroren, die rechte Seite
  ist also glatt, auch für Newton. Danach folgen Zuweisungen und Umschalten.
  Ein sehr kurzer Schritt, in dem alle Stellungen frei dem Newton-Verfahren
  folgen, bringt voneinander abhängige Schaltelemente und die algebraischen
  Unbekannten in Einklang; dann startet die Integration neu.
- **AC**: Linearisierung im Arbeitspunkt, `(G + jωC)·X = B` mit `G = ∂f/∂x`,
  `C = ∂q/∂x`. Gleichungen und Verhaltensquellen sind darin enthalten.
- **Rauschen**: Jede Rauschquelle ist ein Strom zwischen zwei Knoten. Ihre
  Übertragung zum Ausgang liefert eine adjungierte Lösung je Frequenz,
  `(G + jωC)ᵀ·y = e_aus`; die Beiträge werden quadratisch summiert.
- **Gleichungen**: Jede Variable ist eine Unbekannte, jede Gleichung eine Zeile
  desselben Systems wie die Schaltung. `der(...)`-Terme gehen nach `q`, der
  Rest nach `f`; die Jacobi-Einträge kommen aus dem automatischen Differenzieren
  (Rückwärtsmodus).

### Neues Bauteil

Von `Device` ableiten und `stamp()` implementieren: Ströme, die einen Knoten
verlassen, nach `f`, Ladungen nach `q`, die Ableitungen nach `G` und `C`
(`StampContext` hat Hilfsfunktionen dafür). Zusätzliche Unbekannte (innere
Knoten, Zweigströme) werden in `setup()` mit `Circuit::addUnknown()` angelegt.
Danach das Bauteil in `src/io/Netlist.cpp` (`componentKind`, `addComponent`)
eintragen. `devices/Passive.cpp` und `devices/Diode.cpp` sind gute Vorlagen.
Für viele Fälle genügt allerdings eine `B`-Quelle mit Gleichungen.

### Eigenes System in C++

Wer den Kern ohne Eingabedatei nutzen will, leitet von `DaeSystem` ab und
implementiert `size()` und `evaluate()` (liefert `f`, `q` und die
Jacobi-Matrizen als Triplets). Optional sind `eventFunctions()` und
`eventUpdate()` für Ereignisse, `nextBreakpoint()` für Knicke in der Zeit und
`absTol()`. [`examples/dae/Robertson.hpp`](examples/dae/Robertson.hpp)
zeigt das; gelöst wird mit `runTransient()` aus `esimcore`:

    ./build/dae_robertson

## Lizenz

MIT, siehe [`LICENSE`](LICENSE).

## Grenzen und nächste Schritte

- **Index**: Die Verfahren lösen Systeme vom Index 1 und 2; höhere Indizes
  in eigenen Gleichungen werden automatisch reduziert (siehe
  „Indexreduktion“). Nicht reduzierbar sind Bedingungen, deren Ableitung sich
  nicht als Ausdruck bilden lässt: wenn sie Knotenspannungen oder Ströme der
  Schaltung, Tabellen oder Totzeiten enthalten, oder wenn für eine ihrer
  Unbekannten keine Gleichung der Form `c*der(x) = ...` existiert. Dann bleibt
  das System, wie es ist, und eine Warnung nennt die Ursache. Die
  Strukturanalyse betrachtet nur, welche Unbekannte in welcher Gleichung
  vorkommt; heben sich Terme exakt auf, kann der wahre Index höher liegen.
  Systeme vom Index 2 mit `"uic": true` und Startwerten starten, die die
  Bedingungen erfüllen: Ihre Multiplikatoren lassen sich nicht aus den
  algebraischen Gleichungen allein bestimmen, und grob unverträgliche
  Startwerte lassen den ersten Schritt scheitern.
- **Ereignisse**: Gleiten entlang einer Schaltfläche (z. B.
  Reibung als `if`, ein Schalter ohne Hysterese in einer Rückkopplung) führt
  zu endlosem Umschalten und wird abgebrochen; solche Modelle brauchen eine
  Hysterese oder eine geglättete Kennlinie.
- **Schalter und ideale Dioden** haben endliche Widerstände (`ron`, `roff`),
  keine echten Kurz- und Leerläufe. Sehr große Verhältnisse `roff/ron`
  verschlechtern die Kondition der Gleichungen.
- **Genauigkeit**: `reltol` begrenzt den Fehler pro Schritt, nicht den
  Gesamtfehler. Beispiel: Der Kippschwinger trifft seine Periode mit den
  Standardwerten auf 0,004 %, mit `reltol: 1e-9` und `vntol: 1e-10` auf 1 ns.
- **Schwach gedämpfte Schwingungen**: BDF-Formeln der Ordnung 3 bis 5 sind
  dafür nicht stabil genug; die Ordnungssteuerung weicht dann auf niedrige
  Ordnungen aus, die numerisch dämpfen. Für Schwingkreise hoher Güte `trap`
  verwenden.
- **Schaltungen**: Die Halbleitermodelle entsprechen den
  klassischen SPICE-Modellen (Gummel-Poon, MOS Level 1, JFET), dazu ein
  EKV-artiges MOSFET-Modell und BSIM3v3 (ohne dessen Rauschmodelle und
  nicht-quasistatische Effekte; BSIM4 und neuere Modelle fehlen).
- **Große Schaltungen**: Getestet mit bis zu 40 000 Knoten. Ab 1000 Bauteilen
  werden die Bauteile auf mehreren Kernen ausgewertet und die Matrix parallel
  aufgebaut; die Matrixzerlegung bleibt seriell und bestimmt dann die
  Rechenzeit. Der Gewinn ist entsprechend begrenzt: gemessen 26 % bei 20 000
  Dioden (8,2 → 6,0 s) und 27 % bei 3000 Transistoren (15,5 → 11,4 s) auf einem
  Rechner mit zwei schnellen und vier langsamen Kernen. Ruhende
  Schaltungsteile werden nicht übersprungen; die Rechenzeit wächst mit der
  Zahl der Bauteile mal der Zahl der Zeitschritte.
- **Potentialfreie Schaltungsteile** (Sekundärseite eines Transformators,
  Brückengleichrichter) brauchen einen Gleichstrompfad nach Masse, z. B. einen
  großen Widerstand. Ihre Spannungen gegen Masse sind dann nur auf Millivolt
  genau; Differenzen wie `v(plus,minus)` sind davon nicht betroffen.
