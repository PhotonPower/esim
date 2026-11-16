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

## Nächste Schritte

**Schritt 2: Schaltplan**
- Zeichenfläche (`QGraphicsView`): Raster, Zoom, Verschieben, Auswahl,
  Undo/Redo (`QUndoStack`).
- Symbole für alle Bauteiltypen; platzieren, drehen, spiegeln; Leitungen mit
  rechtwinkligem Verlauf; Massesymbol (`"0"`), Netzlabels.
- Eigenschaften des gewählten Bauteils als Formular (Werte mit SI-Präfixen).
- Netzliste aus dem Plan erzeugen: Knotennamen aus Labels, sonst automatisch.
- Plan und Text abgleichen: Auswahl im Plan markiert das Bauteil im Text und umgekehrt.
- Knoten oder Bauteil anklicken, um Spannung oder Strom zu sehen (nach einer Simulation).
- Format unter `"schematic"`: je Bauteilname Position, Drehung, Spiegelung;
  Leitungen als Punktfolgen; Labels.

**Schritt 3**
- Subcircuits als Symbole mit Hierarchie, Modellbibliothek (SPICE-Karten),
  Darstellung von Monte Carlo (Histogramm) und Messgrößen über Sweeps.

**Schritt 4: Windows 11**
- esim und Editor mit MSVC bauen (Pfade mit Umlauten: `std::filesystem::path`),
  `windeployqt`, später ein Installer.

## Offene Punkte

- Speichert der Editor eine handgeschriebene Datei, bleiben ihre Kommentare
  im Text erhalten, solange nur der Text bearbeitet wird. Sobald der
  Schaltplan die Datei schreibt, ist zu klären, wie Kommentare überleben
  (gezieltes Ersetzen der betroffenen Abschnitte statt Neuschreiben).
