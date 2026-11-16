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
| CMake | 4.1.2 (aus Visual Studio); 3.25 kennt VS 2026 nicht |
| Qt | 6.8.3, `msvc2022_64` (über aqtinstall) |
| Python | 3.12.10 |
| ngspice | nicht installiert |

Generator: `Visual Studio 18 2026`. Qt für MSVC 2022 passt dazu, die
Binärschnittstelle ist dieselbe.

## Fehler und Korrekturen

Der Code selbst kompilierte unter MSVC ohne Änderung. Beide Fehler lagen in
der Auslieferung.

| Commit | Fehler | Korrektur |
| --- | --- | --- |
| `e960f89` | `cmake --install … --prefix dist` brach ab: Qts Deploy-Skript verlangt ein absolutes Präfix. Außerdem hätte es die Qt-DLLs nach `bin\` und die Plugins nach `plugins\` gelegt. | `editor/CMakeLists.txt`: Präfix wird absolut gemacht; unter Windows landen DLLs und `platforms\` neben `esim-editor.exe`. |
| `e960f89` | Qt-Dialoge im ausgelieferten Ordner wären englisch gewesen: windeployqt liefert nur `qt_de.qm`, nicht `qtbase_de.qm`. | `editor/main.cpp` lädt zuerst `qt_de`, sonst wie bisher `qtbase_de`. Auf macOS nicht geprüft. |
| `9296a58` | Anleitung und README nannten nur VS 2022. | VS 2026 mit Generator `Visual Studio 18 2026` und CMake ≥ 4.2 ergänzt. |

Das UTF-8-Manifest ist korrekt eingebettet (`activeCodePage` = UTF-8), ohne
Konflikt mit Qts Manifest.

## ctest

6 von 6 Testgruppen bestanden (Release, 7,6 s). Ohne ngspice werden die drei
Vergleichstests nicht angelegt.

| Testgruppe | Ergebnis | Zeit (s) |
| --- | --- | --- |
| esim_tests | bestanden | 1,06 |
| python_interface | bestanden | 0,38 |
| bsim3_model | bestanden | 2,03 |
| spice_import | bestanden | 0,27 |
| editor | bestanden | 0,61 |
| editor_gui | bestanden | 3,14 |

## Prüfliste (Abschnitt 5)

Punkte 1 und 2 sind mit `dist\esim-editor.exe` ohne Qt im PATH geprüft, über
`--run --snapshot`. Die Punkte 3–7 brauchen Maus und Tastatur am Rechner.

| # | Prüfung | Status | Befund |
| --- | --- | --- | --- |
| 1 | Ordner und Datei mit Umlauten | teilweise | `%TEMP%\Übung\Schaltkreis ä.json` öffnet und simuliert; Bild nach `bild ö.png` gesichert. „Datei → Sichern unter“ im Dialog noch nicht. |
| 2 | Beispiel `bjt_amplifier`, Simulation | geprüft | Spannungen an den Labels (2,07 V, 5,35 V, 1,42 V), Frequenzgang mit rund 40 dB. Hinweiszeile zeigt „Strg+D“. |
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
