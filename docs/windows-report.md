# Bericht: esim unter Windows 11

Stand 2026-11-16, Zweig `windows`. Auftrag: `docs/windows-agent.md`.

## Zusammenfassung

esim und der Editor bauen unter Windows 11 mit MSVC fehlerfrei und ohne
Warnungen; alle 6 Testgruppen bestehen. Der Programmordner `dist` läuft ohne
Qt im PATH. Zwei Fehler bei der Auslieferung wurden behoben. Die Punkte 3–7
der Prüfliste sind noch von Hand zu prüfen.

## Werkzeuge

| Werkzeug | Version |
| --- | --- |
| Visual Studio Community 2026 | 18.3.2, MSVC 19.50 (Toolset 14.50) |
| Windows SDK | 10.0.26100.0 |
| CMake | 4.4.4; 3.25 kennt VS 2026 nicht, erst ab 4.2 |
| Qt | 6.8.3, `msvc2022_64` (über aqtinstall) |
| Python | 3.12.10 |
| ngspice | nicht installiert |

CMake wählt ohne `-G` von selbst `Visual Studio 18 2026`. Qt für MSVC 2022
passt dazu, die Binärschnittstelle ist dieselbe.

## Fehler und Korrekturen

Der Code selbst kompilierte unter MSVC ohne Änderung. Beide Fehler lagen in
der Auslieferung.

| Commit | Fehler | Korrektur |
| --- | --- | --- |
| `b4869d3` | `cmake --install … --prefix dist` brach ab: Qts Deploy-Skript verlangt ein absolutes Präfix. Außerdem hätte es die Qt-DLLs nach `bin\` und die Plugins nach `plugins\` gelegt. | `editor/CMakeLists.txt`: Präfix wird absolut gemacht; unter Windows landen DLLs und `platforms\` neben `esim-editor.exe`. |
| `b4869d3` | Qt-Dialoge im ausgelieferten Ordner wären englisch gewesen: windeployqt liefert nur `qt_de.qm`, nicht `qtbase_de.qm`. | `editor/main.cpp` lädt zuerst `qt_de`, sonst wie bisher `qtbase_de`. Auf macOS nicht geprüft. |
| – | Mit CMake 3.25 fand der Build Visual Studio 2026 nicht. | `docs/windows-agent.md`: VS 2026 braucht CMake ≥ 4.2, sonst das mit Visual Studio gelieferte. |

Das UTF-8-Manifest ist korrekt eingebettet (`activeCodePage` = UTF-8), ohne
Konflikt mit Qts Manifest.

## ctest

6 von 6 Testgruppen bestanden (Release, 6,9 s). Ohne ngspice werden die drei
Vergleichstests nicht angelegt.

| Testgruppe | Ergebnis | Zeit (s) |
| --- | --- | --- |
| esim_tests | bestanden | 0,99 |
| python_interface | bestanden | 0,31 |
| bsim3_model | bestanden | 1,94 |
| spice_import | bestanden | 0,22 |
| editor | bestanden | 0,30 |
| editor_gui | bestanden | 3,03 |

## Prüfliste (Abschnitt 5)

Punkte 1 und 2 sind mit `dist\esim-editor.exe` ohne Qt im PATH geprüft, über
`--run --snapshot`. Die Punkte 3–7 brauchen Maus und Tastatur am Rechner.

| # | Prüfung | Status | Befund |
| --- | --- | --- | --- |
| 1 | Ordner und Datei mit Umlauten | teilweise | `%TEMP%\Übung\Schaltkreis ä.json` öffnet und simuliert; Bild nach `bild ö.png` gesichert. „Datei → Sichern unter“ im Dialog noch nicht. |
| 2 | Beispiel `bjt_amplifier`, Simulation | geprüft | Spannungen an den Labels (2,05 V, 5,52 V, 1,39 V, Modell `NPN_GEN`), Frequenzgang mit rund 40 dB. Hinweiszeile zeigt „Strg+D“. |
| 3 | Kontextmenü und Tasten im Plan | offen | |
| 4 | Mausrad, Touchpad, Zoom in den Kurven | offen | |
| 5 | Dialoge auf Deutsch, auch beim Schließen | offen | `qt_de.qm` wird jetzt geladen; Anzeige nicht gesehen. |
| 6 | Skalierung 125 % und 150 %, dunkler Modus | offen | |
| 7 | Herstellerbibliothek hinzufügen und simulieren | offen | |

## Offen

- Prüfliste Punkte 3–7 von Hand durchgehen; bei Fehlern Bildschirmfotos.
- Auf dem Mac prüfen, ob die Qt-Dialoge mit `qt_de` weiter deutsch sind.
- ngspice installieren, damit die drei Vergleichstests laufen.
- `dist/` in `.gitignore` aufnehmen.
