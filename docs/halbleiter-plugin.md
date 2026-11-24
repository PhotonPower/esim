# Halbleiter-Plugin – Spezifikation

Stand: 24.11.2026, Entwurf 1. Noch nichts davon ist umgesetzt. Zielanwendungen:
Optoelektronik, Hall-Sensoren und chemische Sensoren.

## 1. Ziel und Abgrenzung

Numerische Halbleiterbauelemente (Drift-Diffusion auf einem Gitter) werden als
N-Pole monolithisch in das DAE-System eingekoppelt: Die Gitterunbekannten sind
Unbekannte des Gesamtsystems, die Klemmenströme gehen in die Knotengleichungen
ein.

Umgesetzt wird das als **Plugin**: eine eigene Bibliothek (DLL / Shared Object)
mit eigenem Projekt, die esim über die C-Schnittstelle `plugin/esim_plugin.h`
lädt. Der Kern bekommt keine Halbleiter-Klasse; Gitter, Materialdaten und Optik
bleiben im Plugin. Dieselbe Schnittstelle bedient auch Feldlöser (FEM) und andere
Modelle, siehe `docs/fem-plugin-plan.md`.

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

Der Kern löst `d/dt q(x) + f(x, t) = 0` mit `G = df/dx` und `C = dq/dx`, die Form
eines diskretisierten Drift-Diffusions-Modells. Das Plugin beschreibt sein lokales
System in derselben Form; esim hängt es als Bauteil `"type": "plugin"` in die
Schaltung. Die Schnittstelle (`plugin/esim_plugin.h`, reines C) gibt dem lokalen
Vektor fünf Blöcke: Klemmenknoten, Eingänge, Zweigströme, Ausgänge, interne
Unbekannte.

### 3.1 Zuordnung

| Bedarf | In der Schnittstelle |
|---|---|
| Kontakte | Klemmenknoten (`n_nodes`); w enthält die Kontaktspannungen. Das Plugin schreibt in die Zeile des Knotens den Klemmenstrom: Leitungsstrom nach `f`, dielektrische Flussdichte nach `q`. Der Klemmenstrom ist damit konservativ und enthält den Verschiebungsstrom |
| ψ, φn, φp, Ionen, Traps | interne Unbekannte (`n_internal`) mit eigener Toleranz je Unbekannter (`abstol`) und eigener Art (`kinds`: Spannung, Strom, Variable) |
| Poisson-Zeile, Kontaktbedingungen | Zeilen mit `q = 0` (algebraisch) |
| Kontinuitätszeilen | Ladung `±q·n·V_i` nach `q`, Kantenflüsse und Rekombination nach `f` |
| Startwert Arbeitspunkt | `start` in der Beschreibung (z. B. Gleichgewichtslösung bei 0 V, im Plugin berechnet) |
| Licht, Magnetfeld, pH, Temperatur | Eingänge: Ausdrücke der Netzliste (Spannungen, Variablen, Zeit), z. B. `"inputs": {"P_opt": "v(light)", "B": "B0*sin(2*pi*f*t)"}`. Kleinsignalantwort auf Licht: eine Quelle mit `"ac": 1` am Knoten `light`; die Kettenregel in G und C liefert sie ohne weiteren Aufwand |
| Photostrom, Verlustleistung, Hall-Spannung als Größe | Ausgänge: je eine Variable in der Netzliste |
| Newton-Begrenzung | `limit_mode` und `limited` in `esim_eval`: das Plugin dämpft wie SPICE und meldet es |
| Quellenhomotopie | wirkt über die Quellen der Schaltung (Kontaktspannungen, Lichtleistung als Spannung) |
| Rauschen | `noise_rows` und `noise()`: Quellen zwischen zwei Zeilen mit `white` und `flicker` |
| Feldausgabe | `snapshot()`: Bänder, Dichten, Ströme über dem Ort am Ende von Arbeitspunkt und Transient oder zu gegebenen Zeiten (`"snapshots"` am Bauteil) |
| Jacobi-Matrizen | im Plugin, per automatischer Differentiation; esim bekommt das feste Besetzungsmuster einmal in `describe` und die Werte je Auswertung |

### 3.2 Was der Kern liefert und was zu beachten ist

1. **Toleranzen.** Die Fehlerschätzung des Integrators arbeitet auf den Ladungen,
   leitet die Toleranz je Zeile aber aus der Matrix C ab (`tolq = Σ |C| · tolx`,
   `Transient.cpp`). Mit Quasi-Fermi-Potentialen in Volt als Unbekannten und einer
   Spannungstoleranz je Unbekannter skaliert das ohne weiteres über viele
   Größenordnungen der Knotenladungen; mit Dichten als Unbekannten nicht. Die
   Unbekannten sollten daher Potentiale sein (`kinds`: Spannung).
2. **Rauschen.** Die Rauschanalyse löst je Frequenz einmal das adjungierte System; jede
   weitere Quelle kostet ein Skalarprodukt. Tausende Quellen (eine je Gitterkante für
   Diffusionsrauschen, eine je Knoten für Generation-Rekombination) sind kein
   Aufwandsproblem. Das Plugin meldet sie mit festen Zeilenpaaren und liefert die
   Dichten in `noise()` am Arbeitspunkt.
3. **Parameterableitungen.** `sens` arbeitet mit Differenzenquotienten und liest die
   Netzliste neu; Parameter im `config` des Plugins sind damit untersuchbar. Direkte
   oder adjungierte Ableitungen wären eine spätere Erweiterung.
4. **Löser.** Eigen SparseLU mit COLAMD; Besetzungsmuster und Faktorisierung werden
   wiederverwendet. Das reicht für 1D und 2D bis etwa 10⁴ bis 10⁵ Unbekannte; für 3D
   ist eine Elimination der inneren Unbekannten im Plugin nötig (dann gibt das Plugin
   nur Klemmenströme samt Jacobi-Matrix heraus, ohne eigenes Zeitverhalten).
5. **Mehrfaches Anlegen.** `create` wird bei jedem Einlesen der Netzliste aufgerufen:
   Indexreduktion, Parameterstudien, Monte-Carlo, Sensitivitäten. Ein großes Gitter
   gehört daher in einen Zwischenspeicher der Bibliothek (nach Dateiname und
   Konfiguration), nicht in jede Instanz.
6. **Threads.** Ein einzelnes Bauteil wird in einem Thread ausgewertet;
   Parallelität liefert das Plugin intern. Ein Plugin mit Zustand (Begrenzer)
   setzt `thread_safe = 0`.
7. **Arbeitspunktsuche.** gmin-Stepping und Pseudo-Transiente legen einen kleinen
   Leitwert auf die Zeilen der Unbekannten der Art „Spannung“; die Zeile sollte
   dafür etwa die Einheit eines Stroms haben (auf Referenzwerte skaliert).
8. **Eingabeformat.** Geometrie, Regionen, Dotierprofile, Materialien und Kontakte
   stehen in der `config` des Plugins (JSON oder eine eigene Datei, relativ zur
   Netzliste); Zahlen und konstante Ausdrücke der Netzlisten-Parameter werden von esim
   aufgelöst.

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

## 6. Softwarestruktur (der Plugin-Bibliothek)

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
2. **M2 – Kopplung:** Plugin gegen die Schnittstelle (`create`, `describe`, `evaluate`,
   Begrenzung, Schnappschüsse), Transient; Fall A (Fotodiode mit Verstärker),
   Lambert-Beer-Generation. Licht als Eingang.
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

- Grenzen des Lösers messen (Unbekannte, Faktorisierungszeit in 2D); ab wann die
  Elimination der inneren Unbekannten im Plugin nötig wird
- Format der Schnappschüsse (VTK, CSV, JSON)
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
