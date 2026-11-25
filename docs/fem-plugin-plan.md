# Plugins für esim: Feldlöser (FEM), Halbleitersimulator, weitere

Stand: 2026-11-24. Die Schnittstelle (Abschnitt 3) ist implementiert und getestet,
alles danach ist Plan.

Ziel: Ein externer Löser wird als Plugin in esim eingebunden. esim modelliert, was um
das Bauteil herum passiert (Verstärker, Wechselrichter, Regler, Quellen, Last,
Mechanik); das Plugin liefert das Verhalten, das esim nicht selbst kann.

Die Schnittstelle kennt keine Physik. Sie beschreibt ein lokales DAE-System

    d/dt q(w) + f(w, t) = 0

mit Anschlüssen an den Stromkreis. Damit passen unterschiedliche Löser dahinter:

| Plugin | Klemmen | Eingänge | Ausgänge | interne Unbekannte |
|---|---|---|---|---|
| FEM Magnet/Motor/Wirbelstrom | Wicklungsklemmen (Zweigströme) | Position, Winkel, Geschwindigkeit, Temperatur | Kraft, Drehmoment, Verluste | Feld-FHG, Wirbelstromzustände |
| Halbleiter (Drift-Diffusion, TCAD) | Kontakte (Knotenströme, Ladung) | Temperatur, Licht | Verlustleistung | ψ, n, p je Gitterpunkt |
| thermisches 3D-Modell | Wärmeströme als Strom | Verlustleistung | Temperaturen | Temperaturfeld |
| neuronales Netz / Tabelle | beliebig | beliebig | beliebig | keine |

## 1. Beispiel Elektromagnet

    u_k = R_k i_k + dψ_k/dt          Kreisgleichung (esim, Klemmenspannung)
    ψ_k = ψ_k(i, z, Feldzustand)     Feld (Plugin)
    F   = F(i, z, Feldzustand)       Feld (Plugin)
    m z'' = F - F_last - c v         Mechanik (esim, Gleichungsblock)

Die Bewegungsspannung steckt in dψ_k/dt, weil ψ_k von z(t) abhängt. Mechanik und
Ansteuerung bleiben in esim, das Plugin kennt nur das Feld.

## 2. Kopplungsstufen

| Stufe | Idee | Stand |
|---|---|---|
| 0 | Dateien: esim schreibt i(t)/u(t) als CSV, das Feldprogramm liefert Kennfelder ψ(i,z), F(i,z) als CSV, esim liest sie als Tabelle | esim-Seite fertig (2D-Tabellen), CSV-Export des Feldprogramms offen |
| 1 | DLL, Black Box: pro Newton-Iteration `evaluate`, Plugin liefert ψ, F und die Jacobi-Matrizen | **Schnittstelle fertig**, Plugin offen |
| 2 | Monolithisch: das Plugin legt Feld-Freiheitsgrade als interne Unbekannte in esims DAE (M dA/dt + K(A) A = N i, ψ = Nᵀ A) | gleiche Schnittstelle, `n_internal > 0` |
| 2b | Reduzierte Ordnung: Wirbelstromproblem als kleines Zustandsraummodell | als Plugin mit internen Unbekannten möglich |

## 3. Schnittstelle (implementiert)

Quelle der Wahrheit: `plugin/esim_plugin.h` (reines C, nur diese eine Datei nötig).
Zusammenfassung:

- Eine Plugin-DLL exportiert `esim_plugin_entry()` und liefert eine Tabelle
  `esim_plugin_v1` mit `create`, `destroy`, `describe`, `evaluate` (Pflicht) und
  `accept_step`, `next_breakpoint`, `max_step`, `reset`, `last_error` (optional).
- Der lokale Vektor w hat die Blöcke `[Knoten | Eingänge | Zweige | Ausgänge | intern]`:
  - **Knoten**: Klemmen des Bauteils. w enthält die Knotenspannung; das Plugin schreibt
    den Strom, der in das Bauteil fließt (f), und die Ladung (q), wie ein SPICE-Bauteil.
  - **Eingänge**: Ausdrücke aus der Netzliste (Spannungen, Ströme, Variablen, Zeit,
    z. B. `"z"`, `"v(a)-v(b)"`), nur lesbar. Ableitungen: esim wendet die Kettenregel an.
  - **Zweige**: Ströme zwischen zwei Klemmen (Wicklung). esim legt die Unbekannte an und
    addiert den Strom in die Klemmenzeilen, das Plugin liefert die Zweiggleichung.
  - **Ausgänge**: Werte, die das Plugin zurückgibt (Kraft, Verlustleistung); esim legt
    je eine Variable an, die in Gleichungen und Ausgaben nutzbar ist.
  - **intern**: Unbekannte des Plugins (Feld-FHG, Ladungsträgerdichten).
- Das Plugin meldet einmal in `describe` die Anzahl der Unbekannten, Namen, Startwerte,
  Toleranzen und das **feste Besetzungsmuster** von G = df/dw und C = dq/dw. In
  `evaluate` füllt es nur noch die Werte (f, q, G, C) aus t, w, Modus (DC / Start /
  Zeitschritt) und Temperatur.
- **Newton-Begrenzung:** `esim_eval.limit_mode` (aus / merken / Newton) und das Ausgabefeld
  `limited`: das Plugin darf wie SPICE bei einem pn-Übergang an einem zum letzten Iterat
  hin gedämpften Punkt auswerten und meldet das; esim akzeptiert dann keine Konvergenz.
  Der Referenzpunkt ist der einzige Zustand, den `evaluate` ändern darf.
- **Art der Unbekannten:** `esim_description.kinds` (Spannung, Strom, Variable) je
  Zweig, Ausgang und interne Unbekannte. Bei Spannungen nimmt esim die Spannungstoleranz
  und legt bei der Arbeitspunktsuche (gmin-Stepping, Pseudo-Transiente) einen kleinen
  Leitwert auf die Zeile.
- **Rauschquellen:** `n_noise`, `noise_rows` (Stromquelle von Zeile a nach Zeile b, -1 =
  Masse) und die Funktion `noise()`, die am Arbeitspunkt `white` und `flicker` liefert.
  esim rechnet sie mit der adjungierten Lösung zusammen mit den Schaltungsquellen
  (Kosten: ein Gleichungssystem je Frequenz, dazu ein Skalarprodukt je Quelle).
- **Schnappschüsse:** Funktion `snapshot(t, w, dir, tag)`; in der Netzliste am Bauteil
  `"snapshots": true` (am Ende eines Arbeitspunkts und eines Transienten) oder
  `{"times": [...]}` (zusätzlich zu diesen Zeiten, esim landet genau darauf). Die Dateien
  nennt das Plugin selbst; `tag` ist `<stem>.<op | tran.end | tran.t<Zeit>>.<Bauteil>`.
  Nicht in Parameterstudien.
- Erweiterbar: jede Struktur beginnt mit ihrer Größe, neue Felder werden nur
  angehängt; optionale Funktionen dürfen `NULL` sein. Alte Plugins laufen mit neuem
  esim und umgekehrt, solange `api_version` stimmt.
- C-ABI statt C++: Plugin und esim dürfen mit verschiedenen Compilern, Eigen-Versionen
  und in anderen Sprachen gebaut sein.
- Nicht thread-sichere Plugins (`thread_safe = 0`, Standard) werden von esim
  serialisiert aufgerufen.
- Wiederholter Aufruf: `evaluate` muss reiner Funktionsaufruf in (t, w) sein.
  Fortschreibung von Zustand nur in `accept_step`.

### Netzliste

```json
{
  "plugins": {
    "fem": { "library": "itefem_plugin", "config": { "model": "solenoid.json" } }
  },
  "components": [
    { "type": "V", "name": "VS", "nodes": ["a", "0"], "dc": 24 },
    { "type": "plugin", "name": "X1", "plugin": "fem", "nodes": ["a", "0"],
      "config": { "R": "Rcoil" },
      "inputs":  { "z": "z" },
      "outputs": { "F": "Fm" } }
  ],
  "variables": { "z": { "init": 0.01 }, "vz": { "init": 0 } },
  "equations": ["der(z) = vz", "mass*der(vz) = Fm - k*(z - zrest) - c*vz"]
}
```

- `library`: Dateiname ohne oder mit Endung (`.dll`, `.so`, `.dylib`), relativ zur
  Netzliste. Auch direkt am Bauteil möglich: `"library": "itefem_plugin"`.
- `config`: Plugin-Konfiguration, Bauteil überschreibt Plugin-Eintrag. Strings, die
  Zahlen oder konstante Ausdrücke der Netzlisten-Parameter sind (`"4.7k"`, `"Rcoil"`),
  werden zu Zahlen; Dateinamen bleiben Strings. Damit laufen Parameterstudien durch.
- `nodes`: die Klemmen in der Reihenfolge des Plugins. Strom eines Zweigs:
  `i(X1)` (erster Zweig) und `i(X1.coil)` (nach Namen).
- `inputs`: jeder Eingang des Plugins muss belegt sein. `outputs`: Name der Variablen;
  ohne Eintrag heißt sie `X1.<Ausgang>`. Interne Unbekannte heißen `X1.<name>` und
  stehen nicht in der Standardausgabe.
- Beim Laden steht im Lauf die Zeile `note: plugin component X1: <Pfad>`. Eine
  Plugin-DLL ist ausführbarer Code: Netzlisten aus unbekannter Quelle nicht öffnen.

### Was getestet ist (`tests/tests.cpp: testPlugins`, `plugins/mock/`)

- Magnet mit L(z) = L0/(1+z/z0), Position als Eingang, Kraft als Ausgang, Mechanik
  in esim: gleicher Verlauf (z, i, F) wie dasselbe Modell aus esims eigenen Elementen
  (B-Element mit Flussverkettung plus Gleichungen).
- Diode mit Serienwiderstand, Sperrschichtkapazität und internem Unbekannten
  (Halbleiter-Muster: Knotenzeilen, interne Zeile): Arbeitspunkt, AC-Antwort aus
  G und C, Schaltvorgang gegen die Zusammenschaltung aus R, D, C.
- Fehler: Datei fehlt, Plugin unbekannt, falsche Knotenzahl, Eingang fehlt oder
  unbekannt, Plugin lehnt Konfiguration ab.

## 4. Offen in esim

1. ~~2D-Tabellen~~ fertig: `"tables": {"psi": {"x", "y", "z"}}` bzw. `file` mit `zcol`,
   Aufruf `psi(i, z)`, auch in `phi` des B-Elements (README, Abschnitt Tabellen).
2. Optionale Ereignisse im Plugin (`event_functions`, `event_update`) als angehängte
   Felder der Tabelle; heute bleiben Anschläge, Schalter, Sprünge in esim (`when`).
3. Wechselstromanalyse mit frequenzabhängigem Plugin-Anteil (heute nur G + jωC).
4. ~~Rauschquellen, Newton-Begrenzung, Art der Unbekannten, Schnappschüsse~~ fertig
   (Abschnitt 3; Anlass: Prüfung von pull/1).
5. Ausgang der inneren Unbekannten in den Editor, Plugin-Symbol im Schaltplan.
6. ~~Handbuch-Kapitel „Plugins“~~ fertig (`docs/handbuch/kapitel/format-plugins.tex`,
   Zeilen in Bauteilübersicht und Referenz); PDF neu gebaut (MiKTeX).
7. Plugin-Pfade außerhalb der Netzliste (`ESIM_PLUGIN_PATH`) und eine Option
   `--no-plugins`, um Netzlisten ohne Ausführung fremden Codes zu öffnen.

## 5. Anforderungen an einen Feldlöser (ITEFEM)

- Aufrufbare Bibliothek statt Prozess mit Skript: Modell einmal in `create` laden,
  danach nur Parameter ändern (i, z), Faktorisierung und Muster wiederverwenden.
- Je Wicklung ψ_k und die Ableitungen nach i und z, konsistent zu F (gleiche
  Quadratur), bei nichtlinearem Eisen die Tangente aus dem Newton-Verfahren.
- Bewegung ohne Neuvernetzung, sonst sind Ableitungen nach z nicht glatt:
  Netzverformung oder Gleitfläche (Rotation); sonst nur Stufe 0 mit Kennfeldern.
- Wirbelströme: K, M und die Kopplung N getrennt, mit festem Muster (Stufe 2), oder
  ein reduziertes Modell (2b).
- Ausgaben in SI. Nichtglatte Größen (Netzwechsel, Kantenwechsel der Kraft) zerstören
  Newton und Schrittweitenregelung; Kennfelder glätten.

## 6. Anforderungen an einen Halbleitersimulator

- Klemmen = Kontakte. Das Plugin liefert Strom und Ladung je Kontakt (Knotenzeilen),
  esim liefert die Kontaktspannungen. AC-Kleinsignal folgt aus G und C.
- Interne Unbekannte: ψ, n, p (oder Quasi-Fermi-Potentiale) je Gitterpunkt, bei großen
  Gittern nur der Zeilenblock der Innenelimination: das Plugin eliminiert intern und
  liefert nur Klemmenströme samt Jacobi-Matrix (`n_internal = 0`).
- Temperatur: `esim_eval.temperature` ist die Schaltungstemperatur; Selbsterwärmung
  über einen Ausgang (Verlustleistung) und einen Eingang (Temperatur) mit einem
  thermischen Netzwerk in esim.
- Begrenzung: über `limit_mode` und `limited` (Abschnitt 3), Beispiel `pnjlim` in
  `plugins/mock`. Zusätzlich die Exponentialfunktion linear fortsetzen.
- Toleranzen: der Integrator leitet die Toleranz je Ladungszeile aus C und den
  Toleranzen der Unbekannten ab (`tolq = Σ |C| tolx`); mit Quasi-Fermi-Potentialen in
  Volt als Unbekannten passt das ohne Zusatz. Mit Dichten als Unbekannten nicht.
- Gitter und Feldausgabe: `snapshot` (Abschnitt 3); `create` wird bei jedem Einlesen der
  Netzliste aufgerufen (Indexreduktion, Parameterstudien), große Gitter also in der
  Bibliothek zwischenspeichern. Siehe auch `docs/halbleiter-plugin.md` (pull/1).

## 7. Numerik und Fallstricke

- Glatte Kennlinien (C¹); nichtglattes Verhalten (Sättigung mit Knick) über
  `next_breakpoint` oder in esim als Ereignis.
- Spannungsgespeiste Spule mit Sättigung: Startwert des Stroms über `start` im
  `describe`, im DC-Modus magnetostatisch rechnen.
- `max_step` begrenzt den Zeitschritt auf einen Bruchteil der Wirbelstrom-Zeitkonstante.
- Kosten: eine Auswertung je Newton-Iteration. Gegenmittel: Kennfeld im Plugin
  zwischenspeichern, mehrere Wicklungen in einer Auswertung, Jacobi-Matrix
  wiederverwenden (esim tut das bereits, `reuseJacobian`).

## 8. Validierung

1. Mock-Plugins (erledigt, s. o.): prüft die Schnittstelle ohne Feldlöser.
2. Stufe 0 gegen Stufe 1 am selben Hubmagneten.
3. TEAM 28 (Schwebeplatte, Daten im ITEFEM-Repo): Mechanik in esim, Kraft aus FEM,
   Vergleich mit `team28_dynamic.py`.
4. TEAM 23/24 (Kraft/Drehmoment mit Eisen), Transformator/Drossel.
5. Motor: Reluktanz-Prüfstand, dann Dauermagnetmotor mit Wechselrichter und Regler
   (esim: PWM, PI-Regler, `examples/digital_pi_control.json`).
6. Halbleiter: pn-Diode und MOS-Kondensator gegen analytische Modelle.

## 9. Meilensteine

| | Inhalt | Stand |
|---|---|---|
| M0 | CSV-Formate (Wellenform i(t), u(t), z, v; Kennfeld ψ(i,z), F(i,z)), 2D-Tabellen | 2D-Tabellen fertig; Kennfeld als `x,y,z`-CSV festgelegt |
| M1 | Schnittstelle, Lader, `PluginDevice`, Mock-Plugins, Tests | **fertig** |
| M2 | `itefem_plugin.dll` Black Box: ψ, F, Jacobi-Matrizen, nichtlineares Eisen, Bewegung | offen (FEM-Seite) |
| M3 | Wirbelströme: interne Unbekannte oder reduziertes Modell | offen |
| M4 | Motor: Drehung, Mehrphasen, Wechselrichter und Regler in esim | offen |
| M5 | Halbleiter-Plugin (eigenes Projekt) | offen |
| M6 | Editor: Plugin-Bauteil, Handbuch | offen |

## 10. Offene Fragen

Antworten der ITEFEM-Session (2026-11-24):

- Ansteuerung: Zeitbereich (Rückwärts-Euler, Newton je Schritt, nichtlineares Eisen,
  Wirbelströme, 3D-Kantenelemente) läuft; der Strom wird je Quelle als Zeitverlauf
  (Faktor mal Stromdichte, stückweise linear) vorgegeben. Spannungsvorgabe mit
  Widerstand (Stufe 3) fehlt noch; Zeigerrechnung bei fester Frequenz gibt es.
- Flussverkettung ψ_k ist nicht implementiert (geplant: Integral A·t über das
  Wicklungsgebiet). Kennfelder ψ(i,z), F(i,z) heute nur über ein `sweep`-Studium (ein
  Kindprozess je Punkt, CSV). Kraft: Maxwell-Tensor oder Lorentz.
- `itefem_core` existiert als statische Bibliothek, aber der Ablauf liegt in `main.cxx`
  und `main_*.hxx`: keine Zeitschritt-API, kein Zustand über Aufrufe, keine
  wiederverwendbare Faktorisierung (iterativer Löser, Matrix je Iteration neu). Eine DLL
  braucht einen Umbau dieser Abläufe in eine Klasse; zugesagt ist nichts.
- Bewegung: Translation als `motion`-Studium (RK4 außerhalb des FEM, Kraft-Tabellen),
  Rotation mit vorgegebener Drehzahl; keine Bewegungsspannung (v×B), kein bewegtes
  Netz. Die Bewegungsgleichung in esim zu lassen passt dazu.
- Eingabe: JSON-Modell (`--help`, `--check`, `--convert`), Zeitverläufe als Tabellen
  (`"transient"`: `times`, `waveforms`); kein stdin. Ausgabe: Dateien je Zeitschritt und CSV.
- Der Zeitschrittlöser ist neu und noch nicht committet.

Folgerung: Stufe 0 (Kennfelder aus dem `sweep`-Studium als `x,y,z`-CSV, in esim als
2D-Tabelle) ist der einzige Weg, der ohne Umbau auf der ITEFEM-Seite geht. Dafür fehlt
dort ψ_k. Offen:

- Wie bewegt ITEFEM geometrisch (Netzverformung, Gleitfläche, Neuvernetzung)? Nach
  Antwort: gar nicht (Netz fest), Bewegung nur über Kennfelder.
- Lizenz/Veröffentlichung: esim ist öffentlich (`docs/windows-agent.md`), ITEFEM
  soll es auch werden (noch nicht auf GitHub). Plugins bleiben trotzdem eigene
  Projekte, esim enthält nur `plugin/esim_plugin.h`. Beim Upload von ITEFEM prüfen:
  Lizenzen der Quellen (`legacy_src`, Dokumente in `doku/`, Werbematerial) und keine
  absoluten Pfade oder Zugangsdaten im Repo.
