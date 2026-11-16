#pragma once

#include <QHash>
#include <QMap>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QVector>

// The schematic of an input file. The text stays the reference: the
// components and their nodes come from "components", positions, wires,
// labels and ground symbols from the "schematic" section of the same file:
//
//   "schematic": {
//     "components": { "R1": { "at": [4, 0], "rot": 90, "mirror": true } },
//     "wires":   [ [[0, -2], [4, -2], [4, -4]] ],
//     "labels":  [ { "name": "out", "at": [8, -2], "rot": 0 } ],
//     "grounds": [ { "at": [0, 2] } ]
//   }
//
// Coordinates are grid units (integers), y points down, rotations are
// 0, 90, 180 or 270 degrees clockwise. Components without a position (a
// file written by hand) are placed in a free area, with a label at each
// pin that names its node, so that the drawing always connects the same
// nodes as the text.

struct Placement {
    QPoint at;
    int rot = 0;          // degrees clockwise
    bool mirror = false;  // mirrored at the vertical axis before the rotation

    QPoint map(QPoint local) const;
    QPointF map(QPointF local) const;
    QPoint mapDir(QPoint dir) const;  // a direction, without the translation
    QTransform transform() const;     // local grid -> schematic grid
};

struct SComponent {
    int index = -1;       // item of "components" in the text; -1: new
    QString name;
    QString type;         // as written, e.g. "R" or "npn"
    char kind = 0;        // as the simulator: 'r', 'c', 'q', ...
    QStringList nodes;    // as in the text (new components: empty)
    QString summary;      // value shown at the symbol
    QString variant;      // detail of the symbol: "pnp", "pmos", "sin", "i", ...
    QStringList ports;    // subcircuit: its ports; coupling: the two inductors
    QString subcircuit;   // X: the name of the subcircuit
    QString extra;        // further members as JSON ("\"value\": \"1k\""), for new components and copies
    Placement place;
    bool placed = false;  // the position is in the file
    int begin = -1, end = -1;  // its object in the text
};

bool isGroundName(const QString& node);

struct SWire {
    QVector<QPoint> pts;  // corners, at least two; segments are horizontal or vertical
};

struct SLabel {
    QString name;
    QPoint at;
    int rot = 0;  // direction of the text from the anchor: 0 right, 90 down, 180 left, 270 up
};

struct SGround {
    QPoint at;
    int rot = 0;  // 0: the symbol hangs below the anchor
};

struct Schematic {
    QVector<SComponent> components;
    QVector<SWire> wires;
    QVector<SLabel> labels;
    QVector<SGround> grounds;

    QMap<QString, QStringList> subcircuits;  // name -> ports
    QMap<QString, QString> modelTypes;       // model name -> device type ("npn", "pmos", ...), lower case
    QSet<QString> textNodes;                 // all node names of the text
    bool hasSection = false;                 // the file has a "schematic" section

    int find(const QString& name) const;
    bool nameTaken(const QString& name) const { return find(name) >= 0; }
    QString freeName(const QString& prefix) const;  // "R" -> "R3"
    QRect bounds(bool placedOnly = false) const;  // of the drawing, grid units
};

// Reads components and drawing from the text. Fails (with the reason) if the
// text is no valid JSON object; the schematic then keeps its last state.
bool readSchematic(const QString& text, Schematic& s, QString* error = nullptr);

// --- connections ----------------------------------------------------------------

struct Connectivity {
    QVector<QVector<QPoint>> pinPos;  // per component, per pin: grid position
    QVector<QVector<int>> pinNet;     // net index per pin
    QVector<QStringList> pinNodes;    // node name per pin
    QVector<int> wireNet, labelNet, groundNet;
    QStringList netNames;
    QVector<QPoint> junctions;  // three or more connections meet: drawn as a dot
    QVector<QPoint> openEnds;   // wire ends that connect to nothing
    // pins whose node in the text differs from the drawing: (component, pin)
    QVector<QPair<int, int>> conflicts;
    QStringList warnings;
};

// Joins pins, wire corners, labels and ground symbols that touch (a point on
// a wire connects, two wires that merely cross do not), names the nets
// (label, "0" for ground, else the name the pins had in the text, else
// "n1", "n2", ...) and compares with the nodes of the text.
Connectivity connectSchematic(const Schematic& s);

// --- writing ---------------------------------------------------------------------

// The text with the drawing of `after`: nodes, names, removed and new
// components and the "schematic" section are updated in place; all other
// text, comments included, stays as it was. Components of `after` refer to
// the text by their index.
QString writeSchematic(const QString& text, const Schematic& after);

// The "schematic" section as JSON text; `indent` is the indentation of the
// line holding the key.
QString sectionJson(const Schematic& s, const QString& indent);

// Sets (json non-empty) or removes (json empty) member `key` of component
// `index`; the name of the component is changed with writeSchematic.
QString setComponentMember(const QString& text, int index, const QString& key, const QString& json);

// The text range [begin, end) of component `index`, or false.
bool componentRange(const QString& text, int index, int& begin, int& end);

// Index of the component whose text contains position pos, or -1.
int componentAt(const QString& text, int pos);

// Kinds for the palette ----------------------------------------------------------

struct PartTemplate {
    QString id;        // "r", "npn", ...
    QString title;     // German name
    QString type;      // type written into the file
    char kind;
    QString prefix;    // of the name
    QString variant;
    int pins;          // number of nodes
    QString extra;     // default members
};

const QVector<PartTemplate>& partTemplates();
// A new component of the template, not yet placed.
SComponent makeComponent(const PartTemplate& t, const Schematic& s);
// A new instance of a subcircuit of the file.
SComponent makeSubcircuit(const QString& name, const Schematic& s);
