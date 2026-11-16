#pragma once

#include <QPoint>
#include <QRectF>
#include <QString>
#include <QVector>

#include "Schematic.hpp"

class QPainter;

// Circuit symbols in local grid units (y down). Pins lie on grid points, in
// the order of the nodes of the netlist; `dir` points away from the body.
struct SymPin {
    QString name;
    QPoint pos;
    QPoint dir;
};

struct Symbol {
    QVector<SymPin> pins;
    QRectF body;  // the drawing without the pin leads' ends, for hits and text placement
};

Symbol symbolOf(const SComponent& c);

// Text inside a symbol (port names, signs), drawn by the caller upright in
// screen space. Position in local grid units.
struct SymbolText {
    QPointF at;
    QString text;
    int align;  // Qt::Alignment flags relative to `at`
    double size = 0.7;  // height in grid units
};

// Draws the symbol in local grid units with the painter's pen; the caller
// sets the transform (view and placement) and the pen width.
void drawSymbol(QPainter& p, const SComponent& c, QVector<SymbolText>* texts);

// Ground symbol at (0, 0), hanging down.
void drawGround(QPainter& p);
QRectF groundBody();
