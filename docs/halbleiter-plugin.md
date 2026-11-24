# Halbleiter-Plugin – Spezifikation

Stand: 24.11.2026, Entwurf 1. Noch nichts davon ist umgesetzt. Zielanwendungen:
Optoelektronik, Hall-Sensoren und chemische Sensoren.

## 1. Ziel und Abgrenzung

Numerische Halbleiterbauelemente (Drift-Diffusion auf einem Gitter) werden als
N-Pole monolithisch in das DAE-System eingekoppelt: Die Gitterunbekannten sind
Unbekannte des Gesamtsystems, die Klemmenströme gehen in die Knotengleichungen
ein.

Im Umfang:

- Drift-Diffusion für Elektronen und Löcher, Heterostrukturen, Fermi-Dirac-Statistik
- optische Generation als externes Feld
- galvanomagnetischer Transport (Hall), tensorielle Beweglichkeit
- Elektrolytregionen (Ionen als Spezies) und chemisch aktive Grenzflächen
- Analysen: Arbeitspunkt, Transient, Kleinsignal, Rauschen, Parametersensitivität

Nicht im Umfang (vorerst): Hydrodynamik/Energiebilanz, Quantentransport,
Monte Carlo, Prozesssimulation, selbstkonsistente Optik (Laser),
Gittererwärmung.

## 2. Referenzanwendungen

| Nr. | Fall | Dimension | Prüft |
|---|---|---|---|
| A | pin-Fotodiode mit Transimpedanzverstärker | 1D | Kopplung, Transient, AC, Rauschen |
| B | Heterostruktur-Solarzelle / LED | 1D | Bandkanten, Fermi-Dirac, Rekombination |
| C | ISFET-Stapel Elektrolyt–Stern–Oxid–Si mit Kompakt-MOSFET | 1D | Spezies, Grenzflächenmodelle |
| D | Hall-Platte mit Spinning-Current-Beschaltung | 2D | Tensorfluss, Vierpol, Offset |

## 3. Kopplung an den Kern

Der Kern löst bereits `d/dt q(x) + f(x, t) = 0` mit `G = df/dx` und
`C = dq/dx`; das ist genau die Form, die ein diskretisiertes
Drift-Diffusions-Modell liefert. Das Halbleiterbauelement wird eine weitere
Klasse unter `src/circuit/devices/`, abgeleitet von `Device`
(`src/circuit/Device.hpp`).

### 3.1 Was die vorhandene Schnittstelle schon abdeckt

| Bedarf | Vorhanden |
|---|---|
| Gitterunbekannte anmelden | `setup()` mit `Circuit::addUnknown(label, kind, internal = true)` |
| Residuen und Jacobi-Matrizen | `stamp()` mit `addF`, `addQ`, `addG`, `addC` |
| algebraische Zeilen (Poisson) | Zeilen mit `q_i = 0` |
| Startwert Arbeitspunkt | `applyGuess()` (Ladungsneutralität, eingebautes Potential) |
| Newton-Begrenzung | `StampContext::limiting`, `Evaluation::limited` |
| Quellenhomotopie | `srcScale` |
| Kleinsignal | `(G + jωC)` am Arbeitspunkt, Anregung über `stampAc()` |
| automatische Differentiation | `Dual.hpp` |

Zuordnung der Zeilen: Die Kontinuitätsgleichung eines Knotens schreibt die
Ladung `±q·n·V_i` nach `addQ` und die Summe der Kantenflüsse samt
Rekombination nach `addF`. Die Poisson-Zeile schreibt nur nach `addF`. An
einem Kontakt gehen die Residuen der Kontakt-Kontrollvolumen (Leitungsstrom
nach `addF`, dielektrische Flussdichte nach `addQ`) in die Zeile des
Schaltungsknotens; der Klemmenstrom ist damit konservativ und enthält den
Verschiebungsstrom.

### 3.2 Was erweitert werden muss

1. **Art der Unbekannten.** `UnknownKind` kennt `Voltage`, `Current`,
   `Variable`. Potentiale und Quasi-Fermi-Potentiale passen zu `Voltage`;
   Trap- und Oberflächenbesetzungen brauchen `Variable` mit eigener
   Toleranz. Zu prüfen ist, ob die Fehlerschätzung auf den Ladungen `q`
   (siehe `Transient.hpp`) mit den über viele Größenordnungen streuenden
   Knotenladungen eines Gitters sinnvolle Schrittweiten liefert; nötig ist
   wohl eine Toleranz je Kontinuitätszeile aus Referenzdichte mal
   Kontrollvolumen.
2. **Verteilte Rauschquellen.** `NoiseSource` beschreibt eine Stromquelle
   zwischen zwei Knoten mit `white + flicker / f`. Für die
   Impedanzfeld-Methode genügt das, wenn jede Gitterkante eine solche Quelle
   zwischen ihren beiden internen Unbekannten meldet
   (Diffusionsrauschen) und jeder Knoten eine für Generation-Rekombination.
   Zu prüfen: Aufwand der Rauschanalyse bei einigen tausend Quellen
   (adjungierte Lösung statt einer Lösung je Quelle).
3. **Externe Felder.** Generationsrate, Magnetfeld, pH und Temperatur sind
   Parameter des Bauelements mit optionalem Zeitverlauf (`Waveform`) oder
   Ausdruck. Für die Kleinsignalantwort auf Licht braucht `stampAc()` die
   Ableitung des Residuums nach der Lichtleistung als rechte Seite.
4. **Parameterableitungen.** Für Sensitivitäten `∂f/∂p`, `∂q/∂p` je
   Bauelement, falls die vorhandene Empfindlichkeitsanalyse bisher mit
   Differenzenquotienten arbeitet.
5. **Löser.** Blöcke mit einigen tausend Unbekannten und unsymmetrischer
   Struktur; zu prüfen, ob `LinearSolver` dafür genügt und ob eine
   Block-Elimination der internen Unbekannten (Schur-Komplement) als reine
   Löserstrategie lohnt.
6. **Eingabeformat.** Geometrie, Regionen, Dotierprofile, Materialien und
   Kontakte im JSON; Ausgabe von Feldgrößen (Bänder, Dichten, Ströme über
   dem Ort) neben den CSV-Dateien.

## 4. Physikalisches Modell

### 4.1 Grundgleichungen

$$-\nabla\cdot(\varepsilon\nabla\psi) = q\,(p - n + N_D^+ - N_A^-) + q\sum_i z_i c_i + \rho_T$$

$$\frac{\partial n}{\partial t} = \frac{1}{q}\nabla\cdot J_n + G - R,\qquad \frac{\partial p}{\partial t} = -\frac{1}{q}\nabla\cdot J_p + G - R$$

$$J_n = -q\mu_n n\,\nabla\varphi_n,\qquad J_p = -q\mu_p p\,\nabla\varphi_p$$

Unbekannte je Knoten: $\psi, \varphi_n, \varphi_p$ (Quasi-Fermi-Potentiale).
Dichten:

$$n = N_c\,\mathcal F\!\left(\frac{q(\psi-\varphi_n) - E_{c0}}{kT}\right),\qquad p = N_v\,\mathcal F\!\left(\frac{E_{v0} - q(\psi-\varphi_p)}{kT}\right)$$

mit ortsabhängigen Bandkanten $E_{c0}, E_{v0}$ und Zustandsdichten
$N_c, N_v$; $\mathcal F$ ist Boltzmann oder das Fermi-Dirac-Integral der
Ordnung 1/2.

### 4.2 Generation und Rekombination

- SRH, Auger, strahlend; Oberflächen- und Grenzflächenrekombination
- Traps mit eigener Besetzungsdynamik (zusätzliche Unbekannte, tragen zu $\rho_T$ bei)
- Stoßionisation (optional, für Lawinenfotodioden)
- optische Generation $G_{opt}(r,t)$ als externes Feld; Quellen: Lambert-Beer,
  Transfermatrix, externer Optiklöser

### 4.3 Beweglichkeit und Hall-Effekt

Dotierungs- und feldabhängige Beweglichkeit als austauschbare Modellfunktion.
Im Magnetfeld gilt je Trägersorte mit $J_0$ als Strom ohne Feld

$$J = J_0 + \mu^* (J\times B)\;\;\Rightarrow\;\; J = \frac{J_0 + \mu^*\,J_0\times B + \mu^{*2}(J_0\cdot B)\,B}{1+\mu^{*2}B^2}$$

mit $\mu^* = +r_p\mu_p$ für Löcher und $\mu^* = -r_n\mu_n$ für Elektronen
($r$: Hall-Streufaktor). Piezoresistivität wird als symmetrischer
Beweglichkeitstensor $\mu(\sigma_{mech})$ im selben Mechanismus abgebildet.

### 4.4 Elektrolyt

Ionen sind Spezies mit Ladungszahl $z_i$ und Konzentration $c_i$:

$$\frac{\partial c_i}{\partial t} + \nabla\cdot N_i = 0,\qquad N_i = -D_i\left(\nabla c_i + \frac{z_i q}{kT}\,c_i\nabla\psi\right)$$

Im Gleichgewicht alternativ Poisson-Boltzmann (keine Ionenunbekannten).
Sterische Korrektur (Bikerman) als Option der Statistikfunktion.

### 4.5 Grenzflächen und Kontakte

- **Ohmscher Kontakt:** Ladungsneutralität, $\varphi_n=\varphi_p=V_k$
- **Schottky-Kontakt:** Barrierenhöhe, Rekombinationsgeschwindigkeiten
- **Gate-Kontakt** auf Isolator; **Referenzelektrode** im Elektrolyten mit Potentialoffset
- **Heterogrenzfläche:** thermionische Emission (optional, sonst stetige Quasi-Fermi-Potentiale)
- **Stern-Schicht:** ionenfreie dielektrische Schicht
- **Site-Binding** an Oxid/Elektrolyt: mit
  $K_a = [\mathrm{SiO^-}][\mathrm H^+]/[\mathrm{SiOH}]$,
  $K_b = [\mathrm{SiOH}][\mathrm H^+]/[\mathrm{SiOH_2^+}]$ und der
  Oberflächenaktivität $a_s = a_{H,bulk}\exp(-q\psi_0/kT)$

$$\sigma_0 = qN_s\,\frac{a_s^2 - K_aK_b}{a_s^2 + K_b a_s + K_aK_b}$$

  als potentialabhängige Flächenladung in der Poisson-Gleichung. Für Drift
  und Ansprechzeit: Besetzung der Oberflächenplätze als eigene Unbekannte mit
  Ratengleichungen.

## 5. Diskretisierung

- Voronoi-Finite-Volumen auf Delaunay-Gittern (1D, 2D kartesisch und
  rotationssymmetrisch, später 3D)
- Flüsse: Scharfetter-Gummel; für Fermi-Dirac und sterische Statistik ein
  thermodynamisch konsistentes verallgemeinertes Schema
- Assemblierung pro Element (nicht nur pro Kante), damit der Stromvektor
  rekonstruiert und Tensorbeweglichkeiten angewendet werden können
- Jacobi-Matrizen per automatischer Differentiation (`Dual`)
- Skalierung auf thermische Spannung, Referenzdichte und Debye-Länge
- Zeitintegration durch den vorhandenen Integrator

## 6. Softwarestruktur

| Modul | Inhalt |
|---|---|
| Gitter | Knoten, Elemente, Voronoi-Geometrie, Regionen, Ränder |
| Region | Gleichungssatz (Halbleiter, Isolator, Elektrolyt), Materialdaten |
| Spezies | Ladung, Statistik, Beweglichkeit/Diffusion, Flussfunktion |
| Modelle | Rekombination, Generation, Beweglichkeit – je eine AD-fähige Funktion |
| Grenzflächen | eigene Unbekannte, Flächenladungen, Flussbedingungen |
| Kontakte | Randbedingung, Klemmenstrom, Anbindung an Schaltungsknoten |
| Externe Felder | $G_{opt}$, $T$, $B$, mechanische Spannung, pH/Konzentrationen |
| Assemblierung | Aufrufe von `addF`, `addQ`, `addG`, `addC`; Parameterableitungen, Rauschquellen |

## 7. Analysen

- **Arbeitspunkt:** gedämpfter Newton, Fortsetzung über Spannungen und
  Lichtleistung, Gummel-Iteration nur als Startwert
- **Transient:** gekoppelt, Schrittweitensteuerung im Kern
- **Kleinsignal:** $(G + j\omega C)\,\hat x = \hat b$ am Gesamtsystem
- **Rauschen:** Impedanzfeld-Methode über die adjungierte Lösung; lokale
  Quellen für Diffusions- und Generations-Rekombinations-Rauschen, zusammen
  mit den Schaltungsquellen
- **Sensitivität:** direkt und adjungiert nach externen Parametern;
  Hall-Empfindlichkeit in erster Ordnung als $\partial x/\partial B$ bei $B=0$

## 8. Meilensteine

1. **M1 – 1D-Kern:** Gitter, Poisson, Drift-Diffusion mit
   Boltzmann-Statistik, ohmsche Kontakte, SRH/Auger; pn-Diode im
   Arbeitspunkt. Abgleich mit der Shockley-Kennlinie und DEVSIM.
2. **M2 – Kopplung:** Erweiterungen aus 3.2 (Punkte 1, 3, 6), Transient;
   Fall A (Fotodiode mit Verstärker), Lambert-Beer-Generation.
3. **M3 – Heterostrukturen:** Bandkanten, Fermi-Dirac, verallgemeinerter
   Fluss; Fall B. Abgleich mit ChargeTransport.jl.
4. **M4 – AC und Rauschen:** Kleinsignal, Impedanzfeld; Schrotrauschgrenze
   der Fotodiode als Test ($S_I = 2qI$).
5. **M5 – Spezies und Grenzflächen:** Elektrolyt, Stern-Schicht,
   Site-Binding; Fall C. Test: pH-Steilheit gegen das analytische
   Site-Binding-Modell.
6. **M6 – 2D:** Delaunay/Voronoi, elementweise Assemblierung, Sensitivitäten.
7. **M7 – Hall:** Tensorfluss; Fall D. Test: Hall-Spannung einer langen
   Platte gegen $V_H = r\,I B/(q n t)$, Geometriefaktor gegen konforme
   Abbildung.

## 9. Offene Punkte

- die Prüfpunkte aus 3.2 (Toleranzen je Zeile, Aufwand der Rauschanalyse, Löser)
- Gittererzeugung 2D: eigener Generator oder Import (Gmsh, Triangle)
- Zahlendarstellung: reicht `double` bei großer Bandlücke, oder erweiterte
  Genauigkeit für einzelne Größen
- Format für externe Felder (Übergabe von $G_{opt}$ aus Optikprogrammen)
- Materialdatenbank: Umfang und Quellen

## 10. Literatur

Die Angaben sind aus dem Gedächtnis zusammengestellt und vor dem Zitieren zu
prüfen.

- Selberherr: Analysis and Simulation of Semiconductor Devices (1984)
- Grasser, Selberherr: Mixed-mode device simulation, Microelectronics Journal 31 (2000)
- Tischendorf; Selva Soto; Baumanns et al.: Indexanalyse gekoppelter
  Schaltungs-/Bauelementgleichungen
- Farrell, Koprucki, Fuhrmann et al. (WIAS): verallgemeinerte
  Scharfetter-Gummel-Schemata
- Bonani, Ghione: Noise in Semiconductor Devices (Impedanzfeld-Methode)
- Allegretto, Nathan, Baltes: Numerische Analyse von Magnetfeldsensoren
- Yates, Levine, Healy (1974): Site-Binding-Modell; Bergveld: ISFET-Übersichten
- Sanchez: DEVSIM, JOSS 7(70), 2022
