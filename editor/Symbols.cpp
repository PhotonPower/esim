#include "Symbols.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace {

SymPin pin(const char* name, int x, int y, int dx, int dy) { return {QString::fromLatin1(name), QPoint(x, y), QPoint(dx, dy)}; }

// two pins above and below: (0, -2) and (0, 2)
QVector<SymPin> vertical(const char* a, const char* b) { return {pin(a, 0, -2, 0, -1), pin(b, 0, 2, 0, 1)}; }

// output above and below, control pins on the left at (-3, -1) and (-3, 1)
QVector<SymPin> fourPin(const char* a, const char* b) {
    return {pin(a, 0, -2, 0, -1), pin(b, 0, 2, 0, 1), pin("ctrl+", -3, -1, -1, 0), pin("ctrl-", -3, 1, -1, 0)};
}

int subcircuitRows(const SComponent& c) {
    const int n = int(std::max<qsizetype>(c.ports.size(), c.nodes.size()));
    return std::max(1, (n + 1) / 2);
}

void arrow(QPainter& p, QPointF from, QPointF to, double head = 0.35) {
    p.drawLine(from, to);
    const QPointF d = to - from;
    const double len = std::hypot(d.x(), d.y());
    if (len <= 0) return;
    const QPointF u = d / len, n(-u.y(), u.x());
    QPainterPath tip;
    tip.moveTo(to);
    tip.lineTo(to - u * head + n * head * 0.5);
    tip.lineTo(to - u * head - n * head * 0.5);
    tip.closeSubpath();
    p.save();
    p.setBrush(p.pen().color());
    p.drawPath(tip);
    p.restore();
}

// a filled arrow head at `at` pointing along `dir`
void head(QPainter& p, QPointF at, QPointF dir, double size = 0.35) {
    const double len = std::hypot(dir.x(), dir.y());
    const QPointF u = dir / len, n(-u.y(), u.x());
    QPainterPath tip;
    tip.moveTo(at + u * size * 0.5);
    tip.lineTo(at - u * size * 0.5 + n * size * 0.45);
    tip.lineTo(at - u * size * 0.5 - n * size * 0.45);
    tip.closeSubpath();
    p.save();
    p.setBrush(p.pen().color());
    p.drawPath(tip);
    p.restore();
}

void leads(QPainter& p, double inner) {
    p.drawLine(QPointF(0, -2), QPointF(0, -inner));
    p.drawLine(QPointF(0, inner), QPointF(0, 2));
}

void diamond(QPainter& p) {
    QPainterPath d;
    d.moveTo(0, -1.1);
    d.lineTo(0.85, 0);
    d.lineTo(0, 1.1);
    d.lineTo(-0.85, 0);
    d.closeSubpath();
    p.drawPath(d);
    leads(p, 1.1);
}

void plusMinus(QPainter& p, double y = 0.5, double s = 0.22) {
    p.drawLine(QPointF(-s, -y), QPointF(s, -y));
    p.drawLine(QPointF(0, -y - s), QPointF(0, -y + s));
    p.drawLine(QPointF(-s, y), QPointF(s, y));
}

// control pins on the left of four-pin sources and switches
void controlPins(QPainter& p, QVector<SymbolText>* texts) {
    p.drawLine(QPointF(-3, -1), QPointF(-2, -1));
    p.drawLine(QPointF(-3, 1), QPointF(-2, 1));
    p.save();
    QPen dashed = p.pen();
    dashed.setStyle(Qt::DashLine);
    p.setPen(dashed);
    p.drawLine(QPointF(-2, -1), QPointF(-2, 1));
    p.drawLine(QPointF(-2, 0), QPointF(-0.85, 0));
    p.restore();
    if (texts) {
        texts->push_back({QPointF(-2.4, -1.15), "+", Qt::AlignHCenter | Qt::AlignBottom, 0.6});
        texts->push_back({QPointF(-2.4, 0.85), QString::fromUtf8("−"), Qt::AlignHCenter | Qt::AlignBottom, 0.6});
    }
}

}  // namespace

Symbol symbolOf(const SComponent& c) {
    Symbol s;
    switch (c.kind) {
        case 'r': s.pins = vertical("a", "b"); s.body = QRectF(-0.45, -1.3, 0.9, 2.6); break;
        case 'c': s.pins = vertical("a", "b"); s.body = QRectF(-0.9, -0.5, 1.8, 1.0); break;
        case 'l': s.pins = vertical("a", "b"); s.body = QRectF(-0.1, -1.3, 0.6, 2.6); break;
        case 'v': s.pins = vertical("+", "-"); s.body = QRectF(-1.1, -1.1, 2.2, 2.2); break;
        case 'i': s.pins = vertical("from", "to"); s.body = QRectF(-1.1, -1.1, 2.2, 2.2); break;
        case 'd':
        case 'y': s.pins = vertical("anode", "cathode"); s.body = QRectF(-0.7, -0.6, 1.4, 1.2); break;
        case 'b':
        case 'f':
        case 'h': s.pins = vertical("+", "-"); s.body = QRectF(-0.85, -1.1, 1.7, 2.2); break;
        case 'e':
        case 'g':
            s.pins = fourPin("+", "-");
            s.body = QRectF(-2.5, -1.2, 3.35, 2.4);
            break;
        case 's':
            if (c.nodes.size() == 4) {
                s.pins = fourPin("a", "b");
                s.body = QRectF(-2.5, -1.0, 3.3, 2.0);
            } else {
                s.pins = vertical("a", "b");
                s.body = QRectF(-0.3, -1.0, 1.1, 2.0);
            }
            break;
        case 't':
            s.pins = {pin("1+", -3, -1, -1, 0), pin("1-", -3, 1, -1, 0), pin("2+", 3, -1, 1, 0), pin("2-", 3, 1, 1, 0)};
            s.body = QRectF(-2.2, -1.5, 4.4, 2.0);
            break;
        case 'q':
            s.pins = {pin("C", 1, -2, 0, -1), pin("B", -2, 0, -1, 0), pin("E", 1, 2, 0, 1)};
            s.body = QRectF(-1.3, -1.45, 2.8, 2.9);
            break;
        case 'j':
            s.pins = {pin("D", 1, -2, 0, -1), pin("G", -2, 0, -1, 0), pin("S", 1, 2, 0, 1)};
            s.body = QRectF(-1.0, -1.1, 2.2, 2.2);
            break;
        case 'm':
            s.pins = {pin("D", 1, -2, 0, -1), pin("G", -2, 0, -1, 0), pin("S", 1, 2, 0, 1)};
            if (c.nodes.size() == 4) s.pins.push_back(pin("B", 2, 0, 1, 0));
            s.body = QRectF(-1.0, -1.1, 2.2, 2.2);
            break;
        case 'x': {
            const int rows = subcircuitRows(c);
            const int n = int(std::max<qsizetype>(c.ports.size(), c.nodes.size()));
            for (int k = 0; k < n; ++k) {
                const QString name = k < c.ports.size() ? c.ports[k] : QString::number(k + 1);
                const bool left = k < rows;
                const int row = left ? k : k - rows;
                s.pins.push_back({name, QPoint(left ? -3 : 3, -rows + 1 + 2 * row), QPoint(left ? -1 : 1, 0)});
            }
            s.body = QRectF(-2, -rows, 4, 2 * rows);
            break;
        }
        case 'k': s.body = QRectF(-0.6, -0.6, 1.2, 1.2); break;
        default: s.body = QRectF(-1, -1, 2, 2); break;
    }
    return s;
}

QRectF groundBody() { return QRectF(-0.8, 0, 1.6, 1.3); }

void drawGround(QPainter& p) {
    p.drawLine(QPointF(0, 0), QPointF(0, 0.6));
    p.drawLine(QPointF(-0.8, 0.6), QPointF(0.8, 0.6));
    p.drawLine(QPointF(-0.5, 0.9), QPointF(0.5, 0.9));
    p.drawLine(QPointF(-0.2, 1.2), QPointF(0.2, 1.2));
}

void drawSymbol(QPainter& p, const SComponent& c, QVector<SymbolText>* texts) {
    const auto text = [&](QPointF at, const QString& t, int align, double size = 0.7) {
        if (texts) texts->push_back({at, t, align, size});
    };
    switch (c.kind) {
        case 'r':
            p.drawRect(QRectF(-0.45, -1.3, 0.9, 2.6));
            leads(p, 1.3);
            break;
        case 'c':
            p.drawLine(QPointF(-0.9, -0.25), QPointF(0.9, -0.25));
            p.drawLine(QPointF(-0.9, 0.25), QPointF(0.9, 0.25));
            leads(p, 0.25);
            break;
        case 'l': {
            QPainterPath path;
            path.moveTo(0, -1.2);
            for (int k = 0; k < 4; ++k) path.arcTo(QRectF(-0.3, -1.2 + 0.6 * k, 0.6, 0.6), 90, -180);
            p.drawPath(path);
            leads(p, 1.2);
            break;
        }
        case 'v': {
            p.drawEllipse(QPointF(0, 0), 1.1, 1.1);
            leads(p, 1.1);
            if (c.variant == "sin") {
                QPainterPath w;
                w.moveTo(-0.55, 0.1);
                w.cubicTo(-0.3, -0.35, -0.1, -0.35, 0, 0.1);
                w.cubicTo(0.1, 0.55, 0.3, 0.55, 0.55, 0.1);
                p.drawPath(w);
                p.drawLine(QPointF(-0.18, -0.62), QPointF(0.18, -0.62));
                p.drawLine(QPointF(0, -0.8), QPointF(0, -0.44));
            } else if (c.variant == "pulse" || c.variant == "pwl") {
                QPainterPath w;
                w.moveTo(-0.55, 0.45);
                w.lineTo(-0.25, 0.45);
                w.lineTo(-0.25, -0.15);
                w.lineTo(0.25, -0.15);
                w.lineTo(0.25, 0.45);
                w.lineTo(0.55, 0.45);
                p.drawPath(w);
                p.drawLine(QPointF(-0.18, -0.62), QPointF(0.18, -0.62));
                p.drawLine(QPointF(0, -0.8), QPointF(0, -0.44));
            } else {
                plusMinus(p);
            }
            break;
        }
        case 'i':
            p.drawEllipse(QPointF(0, 0), 1.1, 1.1);
            leads(p, 1.1);
            arrow(p, QPointF(0, -0.65), QPointF(0, 0.7));
            break;
        case 'd':
        case 'y': {
            QPainterPath tri;
            tri.moveTo(-0.7, -0.55);
            tri.lineTo(0.7, -0.55);
            tri.lineTo(0, 0.55);
            tri.closeSubpath();
            p.save();
            if (c.kind == 'y') p.setBrush(p.pen().color());
            p.drawPath(tri);
            p.restore();
            p.drawLine(QPointF(-0.7, 0.55), QPointF(0.7, 0.55));
            leads(p, 0.55);
            break;
        }
        case 'b':
            diamond(p);
            if (c.variant == "i") arrow(p, QPointF(0, -0.55), QPointF(0, 0.6), 0.3);
            else if (c.variant == "v") plusMinus(p, 0.45, 0.18);
            else text(QPointF(0, 0), c.variant == "phi" ? QString::fromUtf8("Φ") : "Q", Qt::AlignCenter, 0.75);
            break;
        case 'f':
            diamond(p);
            arrow(p, QPointF(0, -0.55), QPointF(0, 0.6), 0.3);
            break;
        case 'h':
            diamond(p);
            plusMinus(p, 0.45, 0.18);
            break;
        case 'e':
        case 'g':
            diamond(p);
            if (c.kind == 'g') arrow(p, QPointF(0, -0.55), QPointF(0, 0.6), 0.3);
            else plusMinus(p, 0.45, 0.18);
            controlPins(p, texts);
            break;
        case 's': {
            p.drawLine(QPointF(0, -2), QPointF(0, -0.8));
            p.drawLine(QPointF(0, 0.8), QPointF(0, 2));
            p.drawLine(QPointF(0, 0.8), QPointF(0.75, -0.7));
            p.drawEllipse(QPointF(0, -0.8), 0.12, 0.12);
            p.drawEllipse(QPointF(0, 0.8), 0.12, 0.12);
            if (c.nodes.size() == 4) {
                p.drawLine(QPointF(-3, -1), QPointF(-2, -1));
                p.drawLine(QPointF(-3, 1), QPointF(-2, 1));
                p.save();
                QPen dashed = p.pen();
                dashed.setStyle(Qt::DashLine);
                p.setPen(dashed);
                p.drawLine(QPointF(-2, -1), QPointF(-2, 1));
                p.drawLine(QPointF(-2, 0), QPointF(0.4, 0));
                p.restore();
                text(QPointF(-2.4, -1.15), "+", Qt::AlignHCenter | Qt::AlignBottom, 0.6);
                text(QPointF(-2.4, 0.85), QString::fromUtf8("−"), Qt::AlignHCenter | Qt::AlignBottom, 0.6);
            }
            break;
        }
        case 't': {
            // a cable: inner conductor between the + pins, shield to the - pins at each end
            QPainterPath body;
            body.addRoundedRect(QRectF(-2.2, -1.5, 4.4, 1.0), 0.5, 0.5);
            p.drawPath(body);
            p.drawEllipse(QRectF(-2.2, -1.5, 0.5, 1.0));
            p.drawLine(QPointF(-3, -1), QPointF(-2.2, -1));
            p.drawLine(QPointF(2.2, -1), QPointF(3, -1));
            p.drawLine(QPointF(-3, 1), QPointF(-1.5, 1));
            p.drawLine(QPointF(-1.5, 1), QPointF(-1.5, -0.5));
            p.drawLine(QPointF(3, 1), QPointF(1.5, 1));
            p.drawLine(QPointF(1.5, 1), QPointF(1.5, -0.5));
            break;
        }
        case 'q': {
            const bool pnp = c.variant == "pnp";
            p.drawLine(QPointF(-2, 0), QPointF(-0.45, 0));
            p.save();
            QPen bar = p.pen();
            bar.setWidthF(bar.widthF() * 2.2);
            p.setPen(bar);
            p.drawLine(QPointF(-0.45, -0.9), QPointF(-0.45, 0.9));
            p.restore();
            p.drawLine(QPointF(-0.45, -0.4), QPointF(1, -1.2));
            p.drawLine(QPointF(1, -1.2), QPointF(1, -2));
            p.drawLine(QPointF(-0.45, 0.4), QPointF(1, 1.2));
            p.drawLine(QPointF(1, 1.2), QPointF(1, 2));
            const QPointF e0(-0.45, 0.4), e1(1, 1.2);
            if (pnp) head(p, e0 + (e1 - e0) * 0.35, e0 - e1);
            else head(p, e0 + (e1 - e0) * 0.72, e1 - e0);
            p.drawEllipse(QPointF(0.2, 0), 1.45, 1.45);
            break;
        }
        case 'j': {
            const bool pchannel = c.variant == "pjf";
            p.save();
            QPen bar = p.pen();
            bar.setWidthF(bar.widthF() * 2.2);
            p.setPen(bar);
            p.drawLine(QPointF(-0.3, -1.0), QPointF(-0.3, 1.0));
            p.restore();
            p.drawLine(QPointF(-0.3, -0.6), QPointF(1, -0.6));
            p.drawLine(QPointF(1, -0.6), QPointF(1, -2));
            p.drawLine(QPointF(-0.3, 0.6), QPointF(1, 0.6));
            p.drawLine(QPointF(1, 0.6), QPointF(1, 2));
            p.drawLine(QPointF(-2, 0), QPointF(-0.3, 0));
            head(p, QPointF(pchannel ? -1.3 : -0.75, 0), QPointF(pchannel ? -1 : 1, 0));
            break;
        }
        case 'm': {
            const bool pmos = c.variant == "pmos";
            const bool bulkPin = c.nodes.size() == 4;
            p.drawLine(QPointF(-2, 0), QPointF(-0.75, 0));
            p.drawLine(QPointF(-0.75, -0.8), QPointF(-0.75, 0.8));
            p.save();
            QPen bar = p.pen();
            bar.setWidthF(bar.widthF() * 2.2);
            p.setPen(bar);
            p.drawLine(QPointF(-0.35, -1.0), QPointF(-0.35, -0.5));
            p.drawLine(QPointF(-0.35, -0.25), QPointF(-0.35, 0.25));
            p.drawLine(QPointF(-0.35, 0.5), QPointF(-0.35, 1.0));
            p.restore();
            p.drawLine(QPointF(-0.35, -0.75), QPointF(1, -0.75));
            p.drawLine(QPointF(1, -0.75), QPointF(1, -2));
            p.drawLine(QPointF(-0.35, 0.75), QPointF(1, 0.75));
            p.drawLine(QPointF(1, 0.75), QPointF(1, 2));
            p.drawLine(QPointF(-0.35, 0), QPointF(1, 0));
            head(p, QPointF(pmos ? 0.55 : 0.1, 0), QPointF(pmos ? 1 : -1, 0));
            if (bulkPin) p.drawLine(QPointF(1, 0), QPointF(2, 0));
            else p.drawLine(QPointF(1, 0), QPointF(1, 0.75));
            break;
        }
        case 'x': {
            const Symbol s = symbolOf(c);
            p.drawRect(s.body);
            for (const SymPin& sp : s.pins) {
                const bool left = sp.pos.x() < 0;
                p.drawLine(QPointF(sp.pos), QPointF(left ? -2 : 2, sp.pos.y()));
                text(QPointF(left ? -1.75 : 1.75, sp.pos.y()), sp.name,
                     (left ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter, 0.6);
            }
            break;
        }
        case 'k': {
            // two coupled windings: a small pair of arcs
            QPainterPath path;
            path.moveTo(-0.45, -0.6);
            path.arcTo(QRectF(-0.75, -0.6, 0.6, 0.6), 90, -180);
            path.arcTo(QRectF(-0.75, 0.0, 0.6, 0.6), 90, -180);
            path.moveTo(0.45, -0.6);
            path.arcTo(QRectF(0.15, -0.6, 0.6, 0.6), 90, 180);
            path.arcTo(QRectF(0.15, 0.0, 0.6, 0.6), 90, 180);
            p.drawPath(path);
            break;
        }
        default:
            p.drawRect(QRectF(-1, -1, 2, 2));
            text(QPointF(0, 0), "?", Qt::AlignCenter);
            break;
    }
}
