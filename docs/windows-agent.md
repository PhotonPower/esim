# Auftrag: esim und Editor unter Windows 11 bauen und prüfen

Dieses Dokument richtet sich an einen Agenten (oder Menschen) auf einem
Windows-11-PC. Der Code wurde auf macOS entwickelt und für Windows
vorbereitet, aber noch nie mit MSVC gebaut. Ziel: Build ohne Fehler, alle
Tests grün, ein eigenständiger Programmordner, und die Bedienung des Editors
unter Windows geprüft. Fehler werden behoben, ohne macOS zu brechen.

Was schon vorbereitet ist, steht in `docs/editor.md` unter „Schritt 4“ und
in der README unter „Windows 11“.

## 1. Werkzeuge installieren

In einer PowerShell (als normaler Benutzer; winget fragt bei Bedarf nach
Administratorrechten):

    winget install --id Git.Git -e
    winget install --id Kitware.CMake -e
    winget install --id Python.Python.3.12 -e
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

Qt 6 für MSVC ohne Konto, über aqtinstall (danach eine neue Shell öffnen,
damit `pip` und `python` im PATH sind):

    python -m pip install aqtinstall
    python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O C:\Qt

Optional ngspice (für drei Vergleichstests; ohne ngspice werden sie nicht
angelegt): die Windows-Version von ngspice.sourceforge.io entpacken und den
Ordner mit `ngspice_con.exe` in den PATH nehmen.

## 2. Code holen

Das Repository ist privat auf GitHub: https://github.com/PhotonPower/esim.
Einmal bei GitHub anmelden (der Git Credential Manager kommt mit Git für
Windows; alternativ `winget install --id GitHub.cli -e` und `gh auth login`),
dann:

    git clone https://github.com/PhotonPower/esim.git
    cd esim
    git checkout -b windows

Gearbeitet wird auf dem Zweig `windows`, nicht auf `main`.

Ohne Zugang zu GitHub geht es auch mit einem Git-Bundle vom Mac:
`git clone esim.bundle esim`.

Bei jedem Push baut GitHub Actions (`.github/workflows/build.yml`) außerdem
auf Windows (MSVC) und macOS und führt die Tests aus; die Ergebnisse stehen
unter „Actions“ im Repository (`gh run list`, `gh run view --log-failed`).

## 3. Bauen, testen, ausliefern

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
    cmake --build build --config Release --parallel
    ctest --test-dir build -C Release --output-on-failure
    cmake --install build --config Release --prefix dist

Erwartet ohne ngspice: die Testgruppen `esim_tests`, `python_interface`,
`bsim3_model`, `spice_import`, `editor` und `editor_gui`, alle bestanden.
`dist\esim-editor.exe` muss sich danach auf einem Rechner ohne Qt starten
lassen (Qt-DLLs, `platforms\qwindows.dll`, `esim.exe`, `examples\` liegen
daneben).

## 4. Wo es am ehesten hakt

- Kompilierfehler unter MSVC, die clang und gcc durchgelassen haben:
  fehlende `#include` (MSVCs Standardbibliothek bindet weniger mit ein),
  `std::min`/`std::max` mit Makros aus `windows.h` (`NOMINMAX` ist gesetzt),
  implizite Umwandlungen.
- Das Manifest `cmake/utf8.manifest` (UTF-8 als Codepage): CMake reicht
  `.manifest`-Dateien an den Linker; falls es mit Qts eigenem Manifest
  kollidiert (Fehler CVT1100 oder doppelte Ressource), das zusammenführen.
  Prüfen: `mt.exe -inputresource:build\Release\esim.exe;#1 -out:con`
  zeigt `activeCodePage` UTF-8.
- `ctest` und die Qt-DLLs: Die Editor-Tests bekommen das `bin` von Qt über
  `ENVIRONMENT_MODIFICATION` (braucht CMake ≥ 3.22) in den PATH.
- `windeployqt` beim `cmake --install`: deutsche Qt-Übersetzungen
  (`translations\qtbase_de.qm`) sollen mitkommen.

## 5. Von Hand prüfen

Mit `dist\esim-editor.exe`:

1. Ordner und Datei mit Umlauten anlegen, z. B. `C:\Temp\Übung\Schaltkreis ä.json`
   (Datei → Neu, Sichern unter …), dann Strg+R: Die Simulation muss laufen
   und Kurven zeigen.
2. Datei → Beispiele → `bjt_amplifier`, Strg+R: Plan mit Spannungen an den
   Labels, Frequenzgang im Ergebnisfeld.
3. Im Plan: Rechtsklick zeigt „Strg+D“, „Strg+G“, „Entf“ (nicht ⌘);
   Bauteil platzieren (Taste A, „Widerstand“), R dreht, Entf löscht,
   Strg+Z nimmt zurück; Hinweiszeile unten nennt Strg und Alt.
4. Mausrad zoomt im Plan; mit Touchpad: Wischen verschiebt, Spreizen oder
   Strg+Rad zoomt. In den Kurven: Rad zoomt, Ziehen zoomt auf ein Rechteck.
5. Dialoge auf Deutsch („Abbrechen“, „Sichern“), auch die Frage nach
   ungesicherten Änderungen beim Schließen.
6. Bildschirmskalierung 125 % und 150 %: Plan, Kurven und Symbole scharf
   und nicht abgeschnitten; Windows im dunklen Modus: Farben lesbar.
7. Modellbibliothek (Strg+L) → „Bibliotheken …“ → eine SPICE-Datei eines
   Herstellers hinzufügen; Bauteil über die Suche platzieren und simulieren.

## 6. Regeln für Änderungen

- Kommentare und Commit-Nachrichten englisch, Texte der Oberfläche deutsch,
  im Stil des vorhandenen Codes; Plattform-`#ifdef` nur, wo es nicht anders
  geht.
- Was unter Windows geändert wird, darf macOS und Linux nicht brechen.
- Jede Korrektur mit Test, wo möglich; die Testgruppen müssen grün bleiben.
- Commits auf dem Zweig `windows`, hochladen mit

      git push -u origin windows

  Nicht auf `main` pushen; der Zweig wird auf dem Mac geprüft und
  übernommen. Ohne GitHub-Zugang zurück als Bundle:
  `git bundle create windows.bundle main..windows`.

## 7. Bericht

Kurz: was installiert wurde (Versionen von MSVC, CMake, Qt, Python), welche
Fehler auftraten und wie sie behoben wurden, Ergebnis von `ctest`, Ergebnis
der Prüfliste aus Abschnitt 5 (mit Bildschirmfotos, wo etwas nicht stimmt),
und was offen ist.
