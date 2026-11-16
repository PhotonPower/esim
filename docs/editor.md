# Editor: Konzept und Stand

## Ziel

Eine Desktop-Anwendung für esim mit **Schaltplaneditor**, **Texteditor** für
die JSON-Eingabe, **Simulation** und **Kurvenbetrachter**, bequem zu bedienen.
Zielsysteme: zuerst macOS, danach Windows 11. Ein Installer und die
Signierung (keine Apple Developer ID) haben keine Priorität.

Der Text bleibt neben dem Schaltplan gleichberechtigt: Gleichungen,
Ereignisse, Vektoren und Analysen lassen sich nicht sinnvoll zeichnen.

## Entscheidungen

| Frage | Entscheidung | Grund |
|---|---|---|
| Technik | Qt 6 Widgets, C++17, im selben CMake-Projekt (`editor/`) | eine Sprache und ein Build mit esim; nativ auf macOS und Windows |
| Lizenz | MIT; nur LGPL-Teile von Qt (dynamisch gelinkt) | Qt Charts, Qt Graphs, QScintilla, QCustomPlot sind GPL |
| Kurven | eigenes Widget (`PlotWidget`) mit Min/Max-Reduktion je Pixel | Millionen Punkte, keine GPL-Bibliothek |
| Rechnen | esim als eigener Prozess (`QProcess`, `--summary`, `--progress`) | Absturz oder Endlosschleife trifft nicht den Editor; Abbruch = Prozess beenden |
| Prüfen beim Tippen | im Editor-Prozess mit `esimcircuit` (Thread, entprellt) | Fehler sofort, ohne Start |
| Dateiformat | Schaltplan unter `"schematic"` in derselben JSON-Datei | eine Datei, `esim datei.json` läuft weiter; der Simulator ignoriert den Schlüssel |
| Plan und Text | der Text ist maßgeblich; der Plan schreibt gezielte Textänderungen | Kommentare bleiben, eine Undo-Geschichte für beides |
| Zeichenfläche | eigenes Widget statt `QGraphicsView` | der Plan wird nach jeder Textänderung neu aufgebaut; Treffer, Raster und Leitungsführung sind einfacher selbst gemacht |
| Farben | validierte 8-Farben-Palette, eigene Stufen für hell und dunkel; Sweeps als Blauskala | unterscheidbar auch bei Farbfehlsichtigkeit |

## Stand

**Schritt 0 (esim)** – erledigt:
- `"schematic"` wird auf oberster Ebene und in Subcircuits ohne Warnung akzeptiert.
- `setWarningStream()`: Warnungen des Parsers gehen an einen wählbaren Stream (je Thread).
- `RunConfig::progress` und `esim --progress`.
- Zahlen werden unabhängig von der Locale gelesen (`core/ParseNumber.hpp`);
  vorher las ein Prozess mit deutscher Locale (jedes Qt-Programm) `"4.7k"` falsch.

**Schritt 1 (Editor ohne Schaltplan)** – erledigt:
- Texteditor: Hervorhebung, Zeilennummern, Einrückung, ⌘/ für Kommentare,
  Fehler/Warnungen an der Zeile, Liste „Probleme“ (Doppelklick springt hin).
- Simulation mit ⌘R (sichert vorher), ⌘. bricht ab, Fortschritt, Protokoll.
- Ergebnisse: Analysen, Größen, Wertetabelle, Kurven mit Zoom, Fadenkreuz,
  Bildexport; die Auswahl bleibt über neue Läufe erhalten.
- Zuletzt geöffnet, Beispiele, Fensterzustand werden gespeichert.
- Tests: `editor_tests` (alle Beispiele bestehen die Prüfung, Fehlerorte,
  Kurvenaufbereitung, SI-Formatierung).

**Schritt 2 (Schaltplan)** – erledigt:
- Plan (`SchematicView`, eigene Zeichenfläche) links, Text rechts; beide
  bearbeitbar, Ansicht → Schaltplan/Text blendet eines aus.
- Symbole für alle Bauteiltypen (R, C, L, V mit DC/Sinus/Puls, I, D, ideale
  Diode, NPN/PNP, N/P-MOSFET mit 3 oder 4 Anschlüssen, N/P-JFET, E, G, F, H,
  B, Schalter mit 2 oder 4 Anschlüssen, Leitung, Kopplung, Teilschaltungen
  als Kasten mit ihren Ports).
- Platzieren, Drehen, Spiegeln, Verschieben (Leitungen an den Anschlüssen
  gehen mit und bleiben rechtwinklig), Verdoppeln, Löschen, Leitungen mit
  Knick, Masse, Netzlabels, Rechteckauswahl, Kontextmenü.
- Netzliste aus dem Plan: Knoten aus Labels, Masse wird `"0"`, sonst behält
  ein Netz den Namen, den seine Anschlüsse im Text hatten (Ausgaben wie
  `v(out)` bleiben gültig), neue Netze heißen `n1`, `n2`, …
- Plan und Text abgleichen: Auswahl im Plan markiert das Bauteil im Text,
  der Textcursor wählt es im Plan; Eigenschaften als Formular.
- Nach der Simulation: Spannungen des Arbeitspunkts an den Labels und in
  den Tooltips; ⌥-Klick zeigt v(Netz) oder i(Bauteil) als Kurve.
- Tests: Lesen und gezieltes Ändern von JSONC (`JsonSpan`), Plan → Text →
  Plan für alle Beispiele, Netzbildung, Bedienung mit Maus und Tastatur im
  echten Hauptfenster (`editor_gui_tests`).

**Schritt 3a (Teilschaltungen im Plan)** – erledigt:
- Jede Teilschaltung hat einen eigenen Plan unter `"schematic"` in ihrer
  Definition. Doppelklick auf eine Instanz (oder „Teilschaltung öffnen“ im
  Kontextmenü) öffnet ihn; die Leiste über dem Plan und ⌘↑ führen zurück.
  Steht der Textcursor in einem Bauteil einer Teilschaltung, wechselt der
  Plan dorthin.
- Ports sind eigene Symbole (beidseitig spitz, Taste P) und verbinden wie
  Labels über den Namen. Nicht gezeichnete Ports ergänzt der Editor links;
  ein Port mit neuem Namen wird an `"ports"` angehängt (die Instanzen
  bekommen dafür einen Anschluss mehr).
- „Neue Teilschaltung …“ legt eine leere Teilschaltung mit Ports an.
- Kurven und Arbeitspunkt im Plan einer Teilschaltung gelten für eine
  Instanz: `v(out)` wird zu `v(U1.out)`, ein Port zum Knoten außen; bei
  mehreren Instanzen fragt der ⌥-Klick nach, die Werte an den Labels stehen
  nur bei einer einzigen Instanz.

**Schritt 3b (Modellbibliothek)** – erledigt:
- Quellen: die Standardbibliothek (`editor/library/standard.lib`, ins
  Programm eingebaut), die Modelle der Beispiele, eigene Dateien und
  Ordner (Einstellung unter „Bibliotheken …“). SPICE-Dateien liefern ihre
  `.model`-Karten (Fortsetzungszeilen, Kommentare; Modelle innerhalb einer
  `.subckt` und unbekannte Typen werden übergangen), esim-Dateien ihren
  Abschnitt `"models"`.
- Der Kommentar über einer Karte ist ihre Beschreibung.
- Wählen: Dialog „Modellbibliothek“ (⌘L), Suche in „Bauteile“, Feld „model“
  der Eigenschaften (Vorschläge, „…“). Das Modell wird beim Platzieren oder
  Zuweisen in `"models"` kopiert (lange Karten als Liste von Zeilen).
- Standardbibliothek: nur allgemeine Modelle mit Lehrbuchwerten (Dioden,
  Bipolartransistoren, MOSFETs Level 1 und BSIM3 0,18 µm); jedes wird im
  Test von esim gerechnet. Karten von Herstellern sind nicht dabei, weil
  ihre Lizenz für die Weitergabe offen ist; sie kommen aus den Dateien der
  Hersteller, die man als eigene Bibliothek einbindet.

**Schritt 3c (Auswertung über Läufe)** – erledigt:
- Die Messgrößen einer Analyse mit `monte_carlo` oder `param_sweep`
  (Tabelle `*.measure.csv`) haben eigene Darstellungen, wählbar über
  „Darstellung“ und je Analyse gemerkt.
- Monte Carlo: Histogramm je Messgröße als eigenes Feld (eigene Achse,
  Klassen nach Freedman–Diaconis), Mittelwert und ±σ markiert, Statistik
  in der Kopfzeile, Anzahl je Klasse beim Überfahren; oder die Punkte über
  einem gestreuten Parameter (zeigt, welcher Parameter die Streuung macht).
- Sweeps: Messgröße über einem Parameter (Linie mit Punkten, über Dekaden
  logarithmisch); weitere Parameter eines verschachtelten Sweeps ergeben
  eine Kurvenschar auf der Blauskala.

**Schritt 3d (SPICE-Teilschaltungen)** – erledigt:
- esim liest Teilschaltungen in SPICE direkt (`src/io/Spice.cpp`, siehe
  README „Subcircuits in SPICE“); ein Vergleich mit ngspice läuft als Test.
- Die Bibliothek liest `.subckt`-Blöcke aus Herstellerdateien; ein Block
  nimmt die `.model`- und `.subckt`-Definitionen der Datei mit, die er
  benutzt. Platzieren (Suche in „Bauteile“, Dialog) kopiert den Text unter
  `"subcircuits"` in die Datei; der Plan zeigt einen Kasten mit den Ports der
  `.subckt`-Zeile. Doppelklick zeigt den SPICE-Text (eine SPICE-Teilschaltung
  hat keinen eigenen Plan).

**Schritt 3e (Auswahl → Teilschaltung)** – erledigt:
- ⌘G (oder Rechtsklick) macht aus den ausgewählten Bauteilen, Leitungen,
  Labels und Massen eine neue Teilschaltung; abgefragt wird nur der Name.
- Ports sind die Netze, die Ausgewähltes mit dem Rest verbinden (Masse nie),
  von links nach rechts. Die Grenze liegt dort, wo ein nicht ausgewähltes
  Element ein ausgewähltes berührt: Innen sitzt dort der Port, außen ein
  Label mit dem Netznamen, sodass die Verbindungen bleiben.
- Die Bauteile wandern mit ihrem Text (Knoten wie im Plan), die Zeichnung
  mit; an ihrer Stelle steht eine Instanz mit Labels an den Anschlüssen.
  Alles ist eine Textänderung: ⌘Z nimmt es in einem Schritt zurück.
- Test: derselbe Arbeitspunkt vor und nach dem Auslagern (esim gerechnet).

### Wie Plan und Text zusammenhängen

Der Text ist maßgeblich. Nach jeder Textänderung (entprellt) liest der Plan
die Datei neu: Bauteile und Knoten aus `"components"`, Lage, Leitungen,
Labels und Masse aus `"schematic"`. Eine Änderung im Plan erzeugt einen
neuen Plan-Zustand; daraus werden nur die betroffenen Stellen des Texts
ersetzt (`writeSchematic`: Name und `"nodes"` geänderter Bauteile, der
Abschnitt `"schematic"`, gelöschte und neue Bauteile). Der Text wird als
eine Änderung eingespielt (nur der Bereich, der sich unterscheidet), also
ein Schritt in der gemeinsamen Undo-Geschichte. So bleiben Kommentare,
Reihenfolge und Formatierung erhalten – der offene Punkt aus Schritt 1 ist
damit gelöst.

Format:

    "schematic": {
      "components": { "R1": { "at": [4, -2], "rot": 270, "mirror": true } },
      "wires":   [ [[0, -2], [2, -2]] ],
      "labels":  [ { "name": "out", "at": [10, -2], "rot": 0 } ],
      "ports":   [ { "name": "in", "at": [-4, 0] } ],      // nur in Teilschaltungen
      "grounds": [ { "at": [0, 2] } ]
    }

Rasterkoordinaten (ganze Zahlen, y nach unten), Drehung im Uhrzeigersinn.
Ein Punkt auf einer Leitung verbindet (T-Abzweig, Punkt wird gezeichnet),
zwei Leitungen, die sich nur kreuzen, nicht.

Widersprechen sich Plan und Text (der Text verbindet einen Anschluss mit
einem anderen Knoten, als die Leitung im Plan), steht eine Warnung in
„Probleme“ und ein roter Ring am Anschluss. Die nächste Änderung im Plan
schreibt die Knoten des Plans.

## Nächste Schritte

**Schritt 4: Windows 11** – erledigt: gebaut, getestet und von Hand bedient
(Bericht: [`docs/windows-report.md`](windows-report.md)); offen ist nur ein
Installer:
- MSVC: `/utf-8` (Quelltexte mit „Ω“, „„“ …), `/bigobj`, `/permissive-`,
  `NOMINMAX`; kein `M_PI`, keine POSIX-Funktionen; Warte-Befehl des
  Thread-Pools auch für MSVC (`_mm_pause`, `__yield`).
- Umlaute in Pfaden: Manifest mit `activeCodePage` UTF-8 für alle Programme
  (`cmake/utf8.manifest`), esim schaltet die Konsole auf UTF-8; das
  Python-Modul liest und schreibt UTF-8 und findet `esim.exe`.
- Editor: Tastennamen je System (Strg/Alt/Entf statt ⌘/⌥/⌫), Kontextmenü mit
  echten Kürzeln, Touchpads, die nur Winkel melden.
- Auslieferung: `cmake --install` mit `windeployqt`/`macdeployqt`; Tests
  finden die Qt-DLLs über den PATH; ngspice als `ngspice_con`.
- Geprüft: MSVC 19.50 (Visual Studio 2026) ohne Warnungen, alle Tests,
  eigenständiger Programmordner, Umlaute in Pfaden; außerdem GCC/libstdc++.
  `windeployqt` liefert die deutschen Qt-Texte als `qt_de`.

## Offene Punkte

- Kopieren und Einfügen zwischen Dateien, Beschriftungen frei verschieben.
- Die Bedienung ist mit simulierten Ereignissen getestet; Rückmeldungen
  aus der echten Benutzung fehlen noch.
