#include "SchematicView.hpp"

#include <QApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointingDevice>
#include <QToolTip>
#include <QWheelEvent>
#include <cmath>
#include <functional>

#include "Format.hpp"
#include "Keys.hpp"
#include "Symbols.hpp"
#include "Theme.hpp"

namespace {

constexpr double kLabelFont = 0.8;  // grid units
constexpr double kTextFont = 0.75;

QColor accent() { return Theme::series(0); }

QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}
QColor conflictColor() { return Theme::series(7); }

bool onSegment(QPoint p, QPoint a, QPoint b) {
    if (a.x() == b.x()) return p.x() == a.x() && p.y() >= std::min(a.y(), b.y()) && p.y() <= std::max(a.y(), b.y());
    if (a.y() == b.y()) return p.y() == a.y() && p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x());
    return false;
}

double segmentDistance(QPointF p, QPointF a, QPointF b) {
    const QPointF d = b - a;
    const double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0 ? QPointF::dotProduct(p - a, d) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + d * t - p;
    return std::hypot(q.x(), q.y());
}

// drops repeated corners and corners in the middle of a straight run
void normalize(SWire& w) {
    QVector<QPoint> out;
    for (const QPoint& p : w.pts)
        if (out.isEmpty() || out.last() != p) out << p;
    for (int k = 1; k + 1 < out.size();) {
        const QPoint a = out[k - 1], b = out[k], c = out[k + 1];
        if ((a.x() == b.x() && b.x() == c.x()) || (a.y() == b.y() && b.y() == c.y())) out.removeAt(k);
        else ++k;
    }
    w.pts = out;
}

// Moves the end `e` (0 or last) of a wire to `to` and keeps the wire
// rectangular: the neighbouring corner follows, a straight wire gets a corner.
void moveEnd(SWire& w, int e, QPoint to) {
    const int n = int(w.pts.size());
    const QPoint from = w.pts[e];
    if (from == to) return;
    if (n == 2) {
        const QPoint other = w.pts[e == 0 ? 1 : 0];
        w.pts[e] = to;
        if (other.x() != to.x() && other.y() != to.y()) {
            const bool horizontal = other.y() == from.y();
            const QPoint corner = horizontal ? QPoint(to.x(), other.y()) : QPoint(other.x(), to.y());
            w.pts.insert(1, corner);
        }
        return;
    }
    const int nb = e == 0 ? 1 : n - 2;
    const QPoint d = to - from;
    const bool horizontal = w.pts[nb].y() == from.y() && w.pts[nb].x() != from.x();
    if (horizontal) w.pts[nb].ry() += d.y();
    else w.pts[nb].rx() += d.x();
    w.pts[e] = to;
}

// Applies the point map f to the selected items; wire ends attached to them follow.
Schematic relocate(const Schematic& m, const SchematicView::Selection& sel, const std::function<QPoint(QPoint)>& f,
                   int how /* 0 move, 1 rotate, 2 mirror */) {
    Schematic out = m;
    QSet<QPoint> attached;
    for (int k = 0; k < out.components.size(); ++k) {
        SComponent& c = out.components[k];
        if (!sel.comps.contains(c.name)) continue;
        for (const SymPin& p : symbolOf(c).pins) attached.insert(c.place.map(p.pos));
        c.place.at = f(c.place.at);
        if (how == 1) c.place.rot = (c.place.rot + 90) % 360;
        if (how == 2) {
            c.place.mirror = !c.place.mirror;
            c.place.rot = (360 - c.place.rot) % 360;
        }
    }
    for (int k : sel.labels) {
        if (k >= out.labels.size()) continue;
        SLabel& l = out.labels[k];
        attached.insert(l.at);
        l.at = f(l.at);
        if (how == 1) l.rot = (l.rot + 90) % 360;
        if (how == 2 && (l.rot == 0 || l.rot == 180)) l.rot = 180 - l.rot;
    }
    for (int k : sel.grounds) {
        if (k >= out.grounds.size()) continue;
        SGround& g = out.grounds[k];
        attached.insert(g.at);
        g.at = f(g.at);
        if (how == 1) g.rot = (g.rot + 90) % 360;
        if (how == 2 && (g.rot == 90 || g.rot == 270)) g.rot = 360 - g.rot;
    }
    for (int k : sel.wires) {
        if (k >= out.wires.size()) continue;
        for (QPoint& p : out.wires[k].pts) {
            attached.insert(p);
            p = f(p);
        }
    }
    // labels and ground symbols sitting on a pin or wire end that moves go along
    const auto turn = [how](int rot, bool label) {
        if (how == 1) return (rot + 90) % 360;
        if (how == 2) return label ? ((rot == 0 || rot == 180) ? 180 - rot : rot) : ((rot == 90 || rot == 270) ? 360 - rot : rot);
        return rot;
    };
    for (int k = 0; k < out.labels.size(); ++k)
        if (!sel.labels.contains(k) && attached.contains(out.labels[k].at)) {
            out.labels[k].at = f(out.labels[k].at);
            out.labels[k].rot = turn(out.labels[k].rot, true);
        }
    for (int k = 0; k < out.grounds.size(); ++k)
        if (!sel.grounds.contains(k) && attached.contains(out.grounds[k].at)) {
            out.grounds[k].at = f(out.grounds[k].at);
            out.grounds[k].rot = turn(out.grounds[k].rot, false);
        }
    for (int k = 0; k < out.wires.size(); ++k) {
        if (sel.wires.contains(k)) continue;
        SWire& w = out.wires[k];
        const QPoint first = w.pts.first(), last = w.pts.last();
        if (attached.contains(first)) moveEnd(w, 0, f(first));
        if (attached.contains(last)) moveEnd(w, int(w.pts.size()) - 1, f(last));
    }
    for (SWire& w : out.wires) normalize(w);
    return out;
}

QRectF componentBox(const SComponent& c) {
    const Symbol sym = symbolOf(c);
    QRectF r = c.place.transform().mapRect(sym.body);
    for (const SymPin& p : sym.pins) {
        const QPoint g = c.place.map(p.pos);
        r.setLeft(std::min(r.left(), double(g.x())));
        r.setRight(std::max(r.right(), double(g.x())));
        r.setTop(std::min(r.top(), double(g.y())));
        r.setBottom(std::max(r.bottom(), double(g.y())));
    }
    return r;
}

double labelTextWidth(const QString& name) {
    static QHash<QString, double> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return *it;
    QFont f = QApplication::font();
    f.setPixelSize(100);
    const double w = QFontMetricsF(f).horizontalAdvance(name) / 100.0 * kLabelFont;
    cache.insert(name, w);
    return w;
}

// the box of a label's text (grid units)
QRectF labelBox(const SLabel& l) {
    const double w = labelTextWidth(l.name) + (l.port ? 1.4 : 0.8), h = 1.1;
    const QPointF a(l.at);
    switch (l.rot) {
        case 90: return QRectF(a.x() - w / 2, a.y() + 0.5, w, h);
        case 180: return QRectF(a.x() - 0.5 - w, a.y() - h / 2, w, h);
        case 270: return QRectF(a.x() - w / 2, a.y() - 0.5 - h, w, h);
        default: return QRectF(a.x() + 0.5, a.y() - h / 2, w, h);
    }
}

QRectF groundBox(const SGround& g) {
    Placement p;
    p.at = g.at;
    p.rot = g.rot;
    return p.transform().mapRect(groundBody());
}

QString letters(const QString& name) {
    int k = int(name.size());
    while (k > 0 && name[k - 1].isDigit()) --k;
    return name.left(k);
}

}  // namespace

SchematicView::SchematicView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 150);
}

// --- state ------------------------------------------------------------------------

void SchematicView::setSchematic(const Schematic& s) {
    model_ = s;
    conn_ = connectSchematic(model_);
    if (drag_ == Drag::Move || drag_ == Drag::Pending) drag_ = Drag::None;
    moveDelta_ = QPoint();
    display_ = model_;
    displayConn_ = conn_;
    Selection keep;
    for (const QString& n : sel_.comps)
        if (model_.find(n) >= 0) keep.comps.insert(model_.components[model_.find(n)].name);
    for (int k : sel_.wires)
        if (k < model_.wires.size()) keep.wires.insert(k);
    for (int k : sel_.labels)
        if (k < model_.labels.size()) keep.labels.insert(k);
    for (int k : sel_.grounds)
        if (k < model_.grounds.size()) keep.grounds.insert(k);
    sel_ = keep;
    if (mode_ == Mode::Place && ghostKind_ == Ghost::Part && model_.nameTaken(ghost_.name))
        ghost_.name = model_.freeName(letters(ghost_.name));
    update();
}

void SchematicView::setStale(bool stale) {
    if (stale_ == stale) return;
    stale_ = stale;
    update();
}

void SchematicView::resetView() { fitted_ = false; }

void SchematicView::setOperatingPoint(const QHash<QString, double>& values) {
    op_ = values;
    update();
}

void SchematicView::selectComponents(const QStringList& names) {
    Selection s;
    for (const QString& n : names)
        if (model_.find(n) >= 0) s.comps.insert(model_.components[model_.find(n)].name);
    setSelection(s, false);
}

QStringList SchematicView::selectedComponents() const {
    QStringList out;
    for (const SComponent& c : model_.components)
        if (sel_.comps.contains(c.name)) out << c.name;
    return out;
}

void SchematicView::setSelection(const Selection& s, bool notify) {
    const bool changed = s.comps != sel_.comps || s.wires != sel_.wires || s.labels != sel_.labels ||
                         s.grounds != sel_.grounds;
    sel_ = s;
    update();
    if (changed && notify) emit selectionChanged(selectedComponents());
}

bool SchematicView::selected(ItemType type, int index) const {
    switch (type) {
        case ItemType::Component: return index >= 0 && index < display_.components.size() &&
                                          sel_.comps.contains(display_.components[index].name);
        case ItemType::Wire: return sel_.wires.contains(index);
        case ItemType::Label: return sel_.labels.contains(index);
        case ItemType::Ground: return sel_.grounds.contains(index);
        default: return false;
    }
}

// --- modes ------------------------------------------------------------------------

void SchematicView::startPlacing(const SComponent& c) {
    if (!canEdit()) return;
    ghost_ = c;
    ghost_.index = -1;
    ghost_.placed = true;
    ghostKind_ = Ghost::Part;
    mode_ = Mode::Place;
    wirePts_.clear();
    setCursor(Qt::CrossCursor);
    setFocus();
    update();
}

void SchematicView::startGround() {
    if (!canEdit()) return;
    ghostKind_ = Ghost::Ground;
    ghostGround_ = SGround();
    mode_ = Mode::Place;
    setCursor(Qt::CrossCursor);
    setFocus();
    update();
}

void SchematicView::startLabel() {
    if (!canEdit()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Netzlabel"), tr("Name des Knotens:"), QLineEdit::Normal,
                                               ghostLabel_.name.isEmpty() ? QStringLiteral("out") : ghostLabel_.name, &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    ghostLabel_.name = name;
    ghostLabel_.port = false;
    ghostKind_ = Ghost::Label;
    mode_ = Mode::Place;
    setCursor(Qt::CrossCursor);
    setFocus();
    update();
}

void SchematicView::startPort() {
    if (model_.scope.isEmpty()) {
        QToolTip::showText(QCursor::pos(), tr("Ports gibt es nur im Plan einer Teilschaltung."), this);
        return;
    }
    if (!canEdit()) return;
    // the ports not drawn yet come first
    QStringList names;
    for (const QString& p : model_.ports) {
        bool drawn = false;
        for (const SLabel& l : model_.labels) drawn = drawn || (l.port && l.name == p);
        if (!drawn) names << p;
    }
    for (const QString& p : model_.ports)
        if (!names.contains(p)) names << p;
    bool ok = false;
    const QString name = QInputDialog::getItem(this, tr("Port"),
                                               tr("Name des Ports (ein neuer Name erweitert die Ports der Teilschaltung):"),
                                               names, 0, true, &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    ghostLabel_.name = name;
    ghostLabel_.port = true;
    ghostKind_ = Ghost::Label;
    mode_ = Mode::Place;
    setCursor(Qt::CrossCursor);
    setFocus();
    update();
}

void SchematicView::startWire() {
    if (!canEdit()) return;
    mode_ = Mode::Wire;
    wirePts_.clear();
    setCursor(Qt::CrossCursor);
    setFocus();
    update();
}

void SchematicView::cancel() {
    if (mode_ == Mode::Wire && !wirePts_.isEmpty()) {
        if (wirePts_.size() >= 2) finishWire();
        else wirePts_.clear();
        update();
        return;
    }
    if (mode_ != Mode::Select) {
        mode_ = Mode::Select;
        wirePts_.clear();
        unsetCursor();
        update();
        return;
    }
    setSelection(Selection());
}

bool SchematicView::canEdit() {
    if (!stale_) return true;
    QToolTip::showText(QCursor::pos(), tr("Der Text enthält einen Fehler: Der Plan lässt sich erst wieder ändern, "
                                          "wenn der Text gültiges JSON ist."),
                       this);
    return false;
}

void SchematicView::commit(Schematic after, const Selection& selection) {
    for (int k = int(after.wires.size()) - 1; k >= 0; --k) {
        normalize(after.wires[k]);
        if (after.wires[k].pts.size() < 2) after.wires.removeAt(k);
    }
    for (SComponent& c : after.components) c.placed = true;
    model_ = after;
    conn_ = connectSchematic(model_);
    display_ = model_;
    displayConn_ = conn_;
    Selection keep;
    for (const QString& n : selection.comps)
        if (model_.find(n) >= 0) keep.comps.insert(n);
    for (int k : selection.wires)
        if (k < model_.wires.size()) keep.wires.insert(k);
    for (int k : selection.labels)
        if (k < model_.labels.size()) keep.labels.insert(k);
    for (int k : selection.grounds)
        if (k < model_.grounds.size()) keep.grounds.insert(k);
    setSelection(keep);
    update();
    emit edited(model_);
}

// --- editing ------------------------------------------------------------------------

void SchematicView::deleteSelection() {
    if (sel_.isEmpty() || !canEdit()) return;
    Schematic after = model_;
    for (int k = int(after.components.size()) - 1; k >= 0; --k)
        if (sel_.comps.contains(after.components[k].name)) after.components.removeAt(k);
    const auto drop = [](auto& list, const QSet<int>& which) {
        QList<int> idx(which.begin(), which.end());
        std::sort(idx.begin(), idx.end(), std::greater<int>());
        for (int k : idx)
            if (k < list.size()) list.removeAt(k);
    };
    drop(after.wires, sel_.wires);
    drop(after.labels, sel_.labels);
    drop(after.grounds, sel_.grounds);
    commit(after, Selection());
}

void SchematicView::duplicateSelection() {
    if (sel_.isEmpty() || !canEdit()) return;
    Schematic after = model_;
    Selection copies;
    const QPoint offset(4, 4);
    for (const SComponent& c : model_.components) {
        if (!sel_.comps.contains(c.name)) continue;
        SComponent d = c;
        d.index = -1;
        d.name = after.freeName(letters(c.name).isEmpty() ? QStringLiteral("U") : letters(c.name));
        d.nodes = QStringList(QVector<QString>(c.nodes.size()));
        d.place.at += offset;
        after.components << d;
        copies.comps.insert(d.name);
    }
    for (int k : sel_.wires) {
        SWire w = model_.wires[k];
        for (QPoint& p : w.pts) p += offset;
        copies.wires.insert(int(after.wires.size()));
        after.wires << w;
    }
    for (int k : sel_.labels) {
        SLabel l = model_.labels[k];
        l.at += offset;
        copies.labels.insert(int(after.labels.size()));
        after.labels << l;
    }
    for (int k : sel_.grounds) {
        SGround g = model_.grounds[k];
        g.at += offset;
        copies.grounds.insert(int(after.grounds.size()));
        after.grounds << g;
    }
    commit(after, copies);
}

void SchematicView::transformSelection(int how) {
    if (mode_ == Mode::Place) {
        if (ghostKind_ == Ghost::Part) {
            if (how == 0) ghost_.place.rot = (ghost_.place.rot + 90) % 360;
            else {
                ghost_.place.mirror = !ghost_.place.mirror;
                ghost_.place.rot = (360 - ghost_.place.rot) % 360;
            }
        } else if (ghostKind_ == Ghost::Ground) {
            ghostGround_.rot = (ghostGround_.rot + (how == 0 ? 90 : 180)) % 360;
        } else {
            ghostLabel_.rot = (ghostLabel_.rot + (how == 0 ? 90 : 180)) % 360;
        }
        update();
        return;
    }
    if (sel_.isEmpty() || !canEdit()) return;
    // the centre: a single component turns about its origin, else the middle of everything selected
    QPoint centre;
    const QStringList comps = selectedComponents();
    if (comps.size() == 1 && sel_.wires.isEmpty() && sel_.labels.isEmpty() && sel_.grounds.isEmpty()) {
        centre = model_.components[model_.find(comps.first())].place.at;
    } else {
        QRectF r;
        bool any = false;
        const auto add = [&](const QRectF& b) {
            r = any ? r.united(b) : b;
            any = true;
        };
        for (const SComponent& c : model_.components)
            if (sel_.comps.contains(c.name)) add(componentBox(c));
        for (int k : sel_.wires)
            for (const QPoint& p : model_.wires[k].pts) add(QRectF(QPointF(p) - QPointF(0.01, 0.01), QSizeF(0.02, 0.02)));
        for (int k : sel_.labels) add(QRectF(QPointF(model_.labels[k].at), QSizeF(0.01, 0.01)));
        for (int k : sel_.grounds) add(QRectF(QPointF(model_.grounds[k].at), QSizeF(0.01, 0.01)));
        centre = QPoint(qRound(r.center().x()), qRound(r.center().y()));
    }
    const auto f = [centre, how](QPoint p) {
        const QPoint d = p - centre;
        return how == 0 ? centre + QPoint(-d.y(), d.x()) : QPoint(centre.x() - d.x(), p.y());
    };
    commit(relocate(model_, sel_, f, how == 0 ? 1 : 2), sel_);
}

void SchematicView::moveSelection(QPoint delta) {
    if (sel_.isEmpty() || !canEdit()) return;
    commit(relocate(model_, sel_, [delta](QPoint p) { return p + delta; }, 0), sel_);
}

Schematic SchematicView::moved(QPoint delta) const {
    return relocate(model_, sel_, [delta](QPoint p) { return p + delta; }, 0);
}

QVector<QPoint> SchematicView::route(QPoint from, QPoint to) const {
    if (from == to) return {};
    if (from.x() == to.x() || from.y() == to.y()) return {to};
    const QPoint corner = horizontalFirst_ ? QPoint(to.x(), from.y()) : QPoint(from.x(), to.y());
    return {corner, to};
}

QSet<QPoint> SchematicView::connectionPoints() const {
    QSet<QPoint> pts;
    for (const auto& pins : conn_.pinPos)
        for (const QPoint& p : pins) pts.insert(p);
    for (const SLabel& l : model_.labels) pts.insert(l.at);
    for (const SGround& g : model_.grounds) pts.insert(g.at);
    for (const SWire& w : model_.wires)
        for (const QPoint& p : w.pts) pts.insert(p);
    return pts;
}

void SchematicView::finishWire() {
    if (wirePts_.size() < 2 || !canEdit()) {
        wirePts_.clear();
        update();
        return;
    }
    Schematic after = model_;
    after.wires.push_back({wirePts_});
    wirePts_.clear();
    commit(after, Selection());
}

void SchematicView::renameLabel(int index) {
    if (index < 0 || index >= model_.labels.size() || !canEdit()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Netzlabel"), tr("Name des Knotens:"), QLineEdit::Normal,
                                               model_.labels[index].name, &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == model_.labels[index].name) return;
    Schematic after = model_;
    after.labels[index].name = name;
    commit(after, sel_);
}

void SchematicView::contextMenu(QMouseEvent* e) {
    // the item under the mouse becomes the selection, unless it is part of it
    const Hit h = hitAt(e->position());
    if (h.type != ItemType::None && !selected(h.type, h.index)) {
        Selection s;
        if (h.type == ItemType::Component) s.comps.insert(display_.components[h.index].name);
        if (h.type == ItemType::Wire) s.wires.insert(h.index);
        if (h.type == ItemType::Label) s.labels.insert(h.index);
        if (h.type == ItemType::Ground) s.grounds.insert(h.index);
        setSelection(s);
    }
    QMenu menu(this);
    const bool any = !sel_.isEmpty();
    const QStringList comps = selectedComponents();
    if (comps.size() == 1) {
        const QString name = comps.first();
        const SComponent& c = model_.components[model_.find(name)];
        if (c.kind == 'x' && model_.subcircuits.contains(c.subcircuit)) {
            const QString sub = c.subcircuit;
            menu.addAction(model_.spiceSubcircuits.contains(sub) ? tr("SPICE-Text von „%1“ zeigen").arg(sub)
                                                                 : tr("Teilschaltung „%1“ öffnen").arg(sub),
                           this, [this, sub]() { emit openSubcircuit(sub); });
        }
        menu.addAction(tr("Eigenschaften von %1").arg(name), this, [this, name]() { emit componentActivated(name); });
        menu.addAction(tr("Strom i(%1) zeigen").arg(name), this, [this, name]() { emit probe(QStringLiteral("i(%1)").arg(name)); });
    }
    const QString net = netAt(e->position());
    if (!net.isEmpty() && !isGroundName(net) && h.type != ItemType::Component)
        menu.addAction(tr("Spannung v(%1) zeigen").arg(net), this, [this, net]() { emit probe(QStringLiteral("v(%1)").arg(net)); });
    if (h.type == ItemType::Label) menu.addAction(tr("Umbenennen …"), this, [this, h]() { renameLabel(h.index); });
    if (!menu.isEmpty()) menu.addSeparator();
    // the keys shown as the platform writes them
    const auto keyed = [](QAction* a, const QKeySequence& key) {
        a->setShortcut(key);
        a->setShortcutVisibleInContextMenu(true);
        return a;
    };
    keyed(menu.addAction(tr("Drehen"), this, [this]() { transformSelection(0); }), QKeySequence(Qt::Key_R))->setEnabled(any);
    keyed(menu.addAction(tr("Spiegeln"), this, [this]() { transformSelection(1); }), QKeySequence(Qt::Key_X))->setEnabled(any);
    keyed(menu.addAction(tr("Verdoppeln"), this, &SchematicView::duplicateSelection), QKeySequence(Qt::CTRL | Qt::Key_D))
        ->setEnabled(any);
    keyed(menu.addAction(tr("Löschen"), this, &SchematicView::deleteSelection), QKeySequence(QKeySequence::Delete))
        ->setEnabled(any);
    keyed(menu.addAction(tr("In Teilschaltung umwandeln …"), this, &SchematicView::extractRequested),
          QKeySequence(Qt::CTRL | Qt::Key_G))
        ->setEnabled(!comps.isEmpty());
    menu.addSeparator();
    keyed(menu.addAction(tr("Leitung zeichnen"), this, &SchematicView::startWire), QKeySequence(Qt::Key_W));
    keyed(menu.addAction(tr("Masse"), this, &SchematicView::startGround), QKeySequence(Qt::Key_G));
    keyed(menu.addAction(tr("Netzlabel …"), this, &SchematicView::startLabel), QKeySequence(Qt::Key_N));
    if (!model_.scope.isEmpty())
        keyed(menu.addAction(tr("Port …"), this, &SchematicView::startPort), QKeySequence(Qt::Key_P));
    keyed(menu.addAction(tr("Bauteil …"), this, &SchematicView::partChooserRequested), QKeySequence(Qt::Key_A));
    menu.addSeparator();
    keyed(menu.addAction(tr("Alles zeigen"), this, &SchematicView::fitAll), QKeySequence(Qt::Key_F));
    if (!model_.scope.isEmpty())
        keyed(menu.addAction(tr("Zur Hauptschaltung"), this, &SchematicView::leaveRequested), QKeySequence(Qt::CTRL | Qt::Key_Up));
    menu.exec(e->globalPosition().toPoint());
}

// --- hits -----------------------------------------------------------------------

QPoint SchematicView::snap(QPointF px) const {
    const QPointF g = toGrid(px);
    return QPoint(qRound(g.x()), qRound(g.y()));
}

QTransform SchematicView::viewTransform() const {
    return QTransform(scale_, 0, 0, scale_, pan_.x(), pan_.y());
}

SchematicView::Hit SchematicView::hitAt(QPointF px) const {
    const QPointF g = toGrid(px);
    const double tol = std::max(0.35, 5.0 / scale_);
    const Schematic& s = display_;
    for (int k = int(s.labels.size()) - 1; k >= 0; --k) {
        const QRectF box = labelBox(s.labels[k]).adjusted(-0.1, -0.1, 0.1, 0.1);
        if (box.contains(g) || std::hypot(g.x() - s.labels[k].at.x(), g.y() - s.labels[k].at.y()) < tol)
            return {ItemType::Label, k};
    }
    for (int k = int(s.grounds.size()) - 1; k >= 0; --k)
        if (groundBox(s.grounds[k]).adjusted(-0.2, -0.2, 0.2, 0.2).contains(g)) return {ItemType::Ground, k};
    for (int k = int(s.components.size()) - 1; k >= 0; --k) {
        const SComponent& c = s.components[k];
        if (c.place.transform().mapRect(symbolOf(c).body).adjusted(-0.2, -0.2, 0.2, 0.2).contains(g))
            return {ItemType::Component, k};
    }
    for (int k = int(s.wires.size()) - 1; k >= 0; --k) {
        const SWire& w = s.wires[k];
        for (int i = 0; i + 1 < w.pts.size(); ++i)
            if (segmentDistance(g, w.pts[i], w.pts[i + 1]) < tol) return {ItemType::Wire, k};
    }
    for (int k = int(s.components.size()) - 1; k >= 0; --k)
        if (componentBox(s.components[k]).adjusted(-0.3, -0.3, 0.3, 0.3).contains(g)) return {ItemType::Component, k};
    return {};
}

QString SchematicView::netAt(QPointF px) const {
    const QPointF g = toGrid(px);
    const double tol = std::max(0.35, 5.0 / scale_);
    const Schematic& s = display_;
    const Connectivity& c = displayConn_;
    for (int k = 0; k < s.components.size(); ++k)
        for (int p = 0; p < c.pinPos[k].size(); ++p)
            if (std::hypot(g.x() - c.pinPos[k][p].x(), g.y() - c.pinPos[k][p].y()) < tol)
                return c.netNames.value(c.pinNet[k][p]);
    for (int k = 0; k < s.labels.size(); ++k)
        if (labelBox(s.labels[k]).contains(g) ||
            std::hypot(g.x() - s.labels[k].at.x(), g.y() - s.labels[k].at.y()) < tol)
            return c.netNames.value(c.labelNet[k]);
    for (int k = 0; k < s.grounds.size(); ++k)
        if (groundBox(s.grounds[k]).contains(g)) return QStringLiteral("0");
    for (int k = 0; k < s.wires.size(); ++k) {
        const SWire& w = s.wires[k];
        for (int i = 0; i + 1 < w.pts.size(); ++i)
            if (segmentDistance(g, w.pts[i], w.pts[i + 1]) < tol) return c.netNames.value(c.wireNet[k]);
    }
    return QString();
}

SchematicView::Selection SchematicView::selectionIn(const QRectF& r) const {
    Selection s;
    for (const SComponent& c : model_.components)
        if (r.intersects(componentBox(c).adjusted(-0.01, -0.01, 0.01, 0.01))) s.comps.insert(c.name);
    for (int k = 0; k < model_.wires.size(); ++k) {
        const SWire& w = model_.wires[k];
        for (int i = 0; i + 1 < w.pts.size(); ++i) {
            const QRectF seg = QRectF(QPointF(w.pts[i]), QPointF(w.pts[i + 1])).normalized().adjusted(-0.01, -0.01, 0.01, 0.01);
            if (r.intersects(seg)) {
                s.wires.insert(k);
                break;
            }
        }
    }
    for (int k = 0; k < model_.labels.size(); ++k)
        if (r.contains(QPointF(model_.labels[k].at)) || r.intersects(labelBox(model_.labels[k]))) s.labels.insert(k);
    for (int k = 0; k < model_.grounds.size(); ++k)
        if (r.contains(QPointF(model_.grounds[k].at))) s.grounds.insert(k);
    return s;
}

// --- view -------------------------------------------------------------------------

void SchematicView::zoomAt(QPointF px, double factor) {
    const QPointF g = toGrid(px);
    scale_ = std::clamp(scale_ * factor, 2.0, 80.0);
    pan_ = px - g * scale_;
    update();
}

void SchematicView::fitAll() {
    QRect b = model_.bounds();
    if (!b.isValid() || (model_.components.isEmpty() && model_.wires.isEmpty())) {
        scale_ = 14.0;
        pan_ = QPointF(width() / 2.0, height() / 2.0);
        update();
        return;
    }
    QRectF r(b);
    for (const SLabel& l : model_.labels) r = r.united(labelBox(l));
    r.adjust(-2, -2, 4, 2);  // room for names and values
    const double sx = (width() - 40) / std::max(1.0, r.width()), sy = (height() - 40) / std::max(1.0, r.height());
    scale_ = std::clamp(std::min(sx, sy), 3.0, 28.0);
    pan_ = QPointF(width() / 2.0, height() / 2.0) - r.center() * scale_;
    update();
}

// --- events -----------------------------------------------------------------------

void SchematicView::mousePressEvent(QMouseEvent* e) {
    setFocus();
    pressPx_ = e->position();
    pressGrid_ = snap(pressPx_);
    mouseGrid_ = pressGrid_;
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && spaceHeld_)) {
        drag_ = Drag::Pan;
        lastPanPx_ = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (mode_ == Mode::Place) {
        if (e->button() == Qt::RightButton) {
            cancel();
            return;
        }
        if (e->button() != Qt::LeftButton || !canEdit()) return;
        Schematic after = model_;
        if (ghostKind_ == Ghost::Part) {
            SComponent c = ghost_;
            c.place.at = mouseGrid_;
            after.components << c;
            commit(after, Selection());
            ghost_.name = model_.freeName(letters(ghost_.name));
        } else if (ghostKind_ == Ghost::Ground) {
            after.grounds.push_back({mouseGrid_, ghostGround_.rot});
            commit(after, Selection());
        } else {
            after.labels.push_back({ghostLabel_.name, mouseGrid_, ghostLabel_.rot, ghostLabel_.port});
            commit(after, Selection());
        }
        return;
    }
    if (mode_ == Mode::Wire) {
        if (e->button() == Qt::RightButton) {
            cancel();
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        const QPoint p = mouseGrid_;
        if (wirePts_.isEmpty()) {
            wirePts_ << p;
            update();
            return;
        }
        const QVector<QPoint> r = route(wirePts_.last(), p);
        if (r.isEmpty()) return;
        wirePts_ += r;
        bool onWire = false;
        for (const SWire& w : model_.wires)
            for (int i = 0; i + 1 < w.pts.size() && !onWire; ++i) onWire = onSegment(p, w.pts[i], w.pts[i + 1]);
        if (onWire || connectionPoints().contains(p)) finishWire();
        update();
        return;
    }
    if (e->button() == Qt::RightButton) {
        contextMenu(e);
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (e->modifiers() & Qt::AltModifier) {
        const Hit h = hitAt(e->position());
        if (h.type == ItemType::Component) {
            emit probe(QStringLiteral("i(%1)").arg(display_.components[h.index].name));
        } else {
            const QString net = netAt(e->position());
            if (!net.isEmpty() && !isGroundName(net)) emit probe(QStringLiteral("v(%1)").arg(net));
        }
        return;
    }
    const Hit h = hitAt(e->position());
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    if (h.type == ItemType::None) {
        if (!shift) setSelection(Selection());
        drag_ = Drag::Rubber;
        rubber_ = QRectF(pressPx_, pressPx_);
        return;
    }
    Selection s = shift ? sel_ : Selection();
    const bool was = selected(h.type, h.index);
    if (!shift && was) s = sel_;  // drag the whole selection
    const auto toggle = [&](auto& set, auto key) {
        if (shift && was) set.remove(key);
        else set.insert(key);
    };
    switch (h.type) {
        case ItemType::Component: toggle(s.comps, display_.components[h.index].name); break;
        case ItemType::Wire: toggle(s.wires, h.index); break;
        case ItemType::Label: toggle(s.labels, h.index); break;
        case ItemType::Ground: toggle(s.grounds, h.index); break;
        default: break;
    }
    setSelection(s);
    drag_ = shift ? Drag::None : Drag::Pending;
}

void SchematicView::mouseMoveEvent(QMouseEvent* e) {
    const QPointF px = e->position();
    mouseGrid_ = snap(px);
    mouseInside_ = true;
    switch (drag_) {
        case Drag::Pan:
            pan_ += px - lastPanPx_;
            lastPanPx_ = px;
            update();
            return;
        case Drag::Pending:
            if ((px - pressPx_).manhattanLength() < 4) return;
            if (!canEdit()) {
                drag_ = Drag::None;
                return;
            }
            drag_ = Drag::Move;
            [[fallthrough]];
        case Drag::Move: {
            const QPoint delta = mouseGrid_ - pressGrid_;
            if (delta != moveDelta_) {
                moveDelta_ = delta;
                display_ = moved(delta);
                displayConn_ = connectSchematic(display_);
                update();
            }
            return;
        }
        case Drag::Rubber:
            rubber_ = QRectF(pressPx_, px).normalized();
            update();
            return;
        case Drag::None: break;
    }
    if (mode_ != Mode::Select) {
        update();
        return;
    }
    const Hit h = hitAt(px);
    const QString net = netAt(px);
    if (h.type != hover_.type || h.index != hover_.index || net != hoverNet_) {
        hover_ = h;
        hoverNet_ = net;
        update();
        QString tip;
        if (h.type == ItemType::Component) {
            const SComponent& c = display_.components[h.index];
            tip = c.summary.isEmpty() ? c.name : c.name + QString::fromUtf8(" · ") + c.summary;
            const QString i = QStringLiteral("i(%1)").arg(c.name);
            if (op_.contains(i)) tip += QString::fromUtf8("\nArbeitspunkt: %1").arg(formatSI(op_.value(i), "A", 4));
        } else if (!net.isEmpty()) {
            tip = isGroundName(net) ? tr("Masse") : QString::fromUtf8("Knoten „%1“").arg(net);
            const QString v = QStringLiteral("v(%1)").arg(net);
            if (op_.contains(v)) tip += QString::fromUtf8(" = %1").arg(formatSI(op_.value(v), "V", 4));
        }
        if (tip.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(e->globalPosition().toPoint(), tip, this);
    }
}

void SchematicView::mouseReleaseEvent(QMouseEvent* e) {
    switch (drag_) {
        case Drag::Pan:
            drag_ = Drag::None;
            if (spaceHeld_) setCursor(Qt::OpenHandCursor);
            else if (mode_ == Mode::Select) unsetCursor();
            else setCursor(Qt::CrossCursor);
            return;
        case Drag::Move: {
            drag_ = Drag::None;
            const QPoint delta = moveDelta_;
            moveDelta_ = QPoint();
            if (delta != QPoint()) {
                commit(display_, sel_);
            } else {
                display_ = model_;
                displayConn_ = conn_;
                update();
            }
            return;
        }
        case Drag::Rubber: {
            drag_ = Drag::None;
            const QRectF g = QRectF(toGrid(rubber_.topLeft()), toGrid(rubber_.bottomRight()));
            rubber_ = QRectF();
            if (g.width() * scale_ > 3 || g.height() * scale_ > 3) {
                Selection s = (e->modifiers() & Qt::ShiftModifier) ? sel_ : Selection();
                const Selection in = selectionIn(g);
                s.comps.unite(in.comps);
                s.wires.unite(in.wires);
                s.labels.unite(in.labels);
                s.grounds.unite(in.grounds);
                setSelection(s);
            }
            update();
            return;
        }
        default: drag_ = Drag::None; break;
    }
}

void SchematicView::mouseDoubleClickEvent(QMouseEvent* e) {
    if (mode_ == Mode::Wire) {
        finishWire();
        return;
    }
    if (mode_ != Mode::Select || e->button() != Qt::LeftButton) return;
    const Hit h = hitAt(e->position());
    if (h.type == ItemType::Component) {
        const SComponent& c = display_.components[h.index];
        if (c.kind == 'x' && model_.subcircuits.contains(c.subcircuit)) emit openSubcircuit(c.subcircuit);
        else emit componentActivated(c.name);
    } else if (h.type == ItemType::Label) {
        renameLabel(h.index);
    }
}

void SchematicView::wheelEvent(QWheelEvent* e) {
    const QPoint pixel = e->pixelDelta(), angle = e->angleDelta();
    const bool touchpad = (e->pointingDevice() && e->pointingDevice()->type() == QInputDevice::DeviceType::TouchPad) ||
                          (!pixel.isNull() && e->phase() != Qt::NoScrollPhase);
    if ((e->modifiers() & Qt::ControlModifier) || !touchpad) {
        const double steps = !pixel.isNull() && touchpad ? pixel.y() / 50.0 : angle.y() / 120.0;
        zoomAt(e->position(), std::pow(1.2, steps));
    } else {
        // Windows touchpads may give angles only
        pan_ += pixel.isNull() ? QPointF(angle) / 4.0 : QPointF(pixel);
        update();
    }
    e->accept();
}

bool SchematicView::event(QEvent* e) {
    if (e->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomAt(g->position(), 1.0 + g->value());
            return true;
        }
        if (g->gestureType() == Qt::SmartZoomNativeGesture) {
            fitAll();
            return true;
        }
    }
    if (e->type() == QEvent::ShortcutOverride) {
        // single keys of the drawing go to the drawing, not to menu shortcuts
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace || k->key() == Qt::Key_Escape ||
            (k->modifiers() == Qt::ControlModifier && k->key() == Qt::Key_D)) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void SchematicView::keyPressEvent(QKeyEvent* e) {
    const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat() && plain) {
        spaceHeld_ = true;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (e->modifiers() == Qt::ControlModifier && e->key() == Qt::Key_D) {
        duplicateSelection();
        return;
    }
    if ((e->modifiers() & Qt::ControlModifier) && e->key() == Qt::Key_Up) {
        if (!model_.scope.isEmpty()) emit leaveRequested();
        return;
    }
    const int step = (e->modifiers() & Qt::ShiftModifier) ? 5 : 1;
    switch (e->key()) {
        case Qt::Key_Escape: cancel(); return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (mode_ == Mode::Wire && !wirePts_.isEmpty()) {
                // take back the last corner
                wirePts_.removeLast();
                update();
            } else {
                deleteSelection();
            }
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (mode_ == Mode::Wire) finishWire();
            return;
        case Qt::Key_Left: moveSelection(QPoint(-step, 0)); return;
        case Qt::Key_Right: moveSelection(QPoint(step, 0)); return;
        case Qt::Key_Up: moveSelection(QPoint(0, -step)); return;
        case Qt::Key_Down: moveSelection(QPoint(0, step)); return;
        default: break;
    }
    if (!plain) {
        QWidget::keyPressEvent(e);
        return;
    }
    switch (e->key()) {
        case Qt::Key_R: transformSelection(0); return;
        case Qt::Key_X: transformSelection(1); return;
        case Qt::Key_W: startWire(); return;
        case Qt::Key_G: startGround(); return;
        case Qt::Key_N: startLabel(); return;
        case Qt::Key_P: startPort(); return;
        case Qt::Key_F: fitAll(); return;
        case Qt::Key_A: emit partChooserRequested(); return;
        case Qt::Key_Slash:
            horizontalFirst_ = !horizontalFirst_;
            update();
            return;
        default: QWidget::keyPressEvent(e);
    }
}

void SchematicView::keyReleaseEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        spaceHeld_ = false;
        if (drag_ != Drag::Pan) {
            if (mode_ == Mode::Select) unsetCursor();
            else setCursor(Qt::CrossCursor);
        }
        return;
    }
    QWidget::keyReleaseEvent(e);
}

void SchematicView::leaveEvent(QEvent*) {
    mouseInside_ = false;
    hover_ = Hit();
    hoverNet_.clear();
    update();
}

void SchematicView::focusOutEvent(QFocusEvent* e) {
    spaceHeld_ = false;
    QWidget::focusOutEvent(e);
    update();
}

void SchematicView::changeEvent(QEvent* e) {
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) update();
    QWidget::changeEvent(e);
}

// --- painting ---------------------------------------------------------------------

double SchematicView::penWidth() const { return std::clamp(scale_ * 0.11, 1.0, 3.0); }

double SchematicView::fontPixels(double gridSize) const { return gridSize * scale_; }

void SchematicView::paintText(QPainter& p, QPointF at, const QString& text, int align, double size, const QColor& color) {
    const double px = fontPixels(size);
    if (px < 5.0 || text.isEmpty()) return;
    QFont f = font();
    f.setPixelSize(std::max(1, qRound(px)));
    p.setFont(f);
    p.setPen(color);
    QRectF r(0, 0, 4000, 4000);
    if (align & Qt::AlignLeft) r.moveLeft(at.x());
    else if (align & Qt::AlignRight) r.moveRight(at.x());
    else r.moveLeft(at.x() - 2000);
    if (align & Qt::AlignTop) r.moveTop(at.y());
    else if (align & Qt::AlignBottom) r.moveBottom(at.y());
    else r.moveTop(at.y() - 2000);
    p.drawText(r, align, text);
}

void SchematicView::paintGrid(QPainter& p) {
    int step = 1;
    if (scale_ < 6) step = 5;
    if (scale_ < 2.5) step = 10;
    const QPointF g0 = toGrid(QPointF(0, 0)), g1 = toGrid(QPointF(width(), height()));
    const int x0 = int(std::floor(g0.x() / step)) * step, y0 = int(std::floor(g0.y() / step)) * step;
    QColor dot = Theme::textSecondary();
    dot.setAlpha(Theme::dark() ? 70 : 60);
    QPen pen(dot, std::clamp(scale_ * 0.09, 1.0, 2.0));
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    QVector<QPointF> pts;
    for (int y = y0; y <= g1.y(); y += step)
        for (int x = x0; x <= g1.x(); x += step) pts << toScreen(QPointF(x, y));
    p.drawPoints(pts.constData(), int(pts.size()));
}

void SchematicView::paintComponent(QPainter& p, const SComponent& c, const QColor& ink, bool, const QVector<int>& conflicts) {
    const QTransform place = c.place.transform();
    p.save();
    p.setTransform(place * viewTransform());
    QPen pen(ink, penWidth() / scale_);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QVector<SymbolText> texts;
    drawSymbol(p, c, &texts);
    p.restore();
    // texts upright; left and right swap when the symbol is turned around
    const bool flipped = (c.place.rot == 180) != c.place.mirror;
    const bool sideways = c.place.rot == 90 || c.place.rot == 270;
    for (const SymbolText& t : texts) {
        int align = t.align;
        if (sideways) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignHCenter;
        else if (flipped && (align & (Qt::AlignLeft | Qt::AlignRight)))
            align = (align & ~Qt::AlignHorizontal_Mask) | ((align & Qt::AlignLeft) ? Qt::AlignRight : Qt::AlignLeft);
        paintText(p, toScreen(place.map(t.at)), t.text, align, t.size, ink);
    }
    // name and value: right of upright symbols, above lying ones
    const Symbol sym = symbolOf(c);
    const QRectF box = place.mapRect(sym.body);
    const QColor nameColor = ink == Theme::textPrimary() ? Theme::textSecondary() : ink;
    // the first pin points up or down (two-pole parts, transistors): text on the right
    const bool upright = sym.pins.isEmpty() || c.place.mapDir(sym.pins.first().dir).x() == 0;
    if (upright) {
        // a pin on the right (bulk of a MOSFET): the text moves up, out of its way
        bool rightPin = false;
        for (const SymPin& pin : sym.pins) rightPin = rightPin || c.place.mapDir(pin.dir).x() > 0;
        const QPointF a(box.right() + 0.45, box.center().y() - (rightPin ? 1.6 : 0.0));
        paintText(p, toScreen(a - QPointF(0, 0.05)), c.name, Qt::AlignLeft | Qt::AlignBottom, kTextFont, nameColor);
        paintText(p, toScreen(a + QPointF(0, 0.05)), c.summary, Qt::AlignLeft | Qt::AlignTop, kTextFont, ink);
    } else {
        const QPointF a(box.center().x(), box.top() - 0.25);
        paintText(p, toScreen(a), c.summary, Qt::AlignHCenter | Qt::AlignBottom, kTextFont, ink);
        paintText(p, toScreen(a - QPointF(0, c.summary.isEmpty() ? 0 : 0.95)), c.name, Qt::AlignHCenter | Qt::AlignBottom,
                  kTextFont, nameColor);
    }
    for (int pin : conflicts) {
        if (pin >= sym.pins.size()) continue;
        p.setPen(QPen(conflictColor(), std::max(1.5, penWidth())));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(toScreen(c.place.map(sym.pins[pin].pos)), 0.45 * scale_, 0.45 * scale_);
    }
}

void SchematicView::paintLabel(QPainter& p, const SLabel& l, const QColor& ink, const QString& extra) {
    const QRectF box = labelBox(l);
    const QPointF a(l.at);
    QPainterPath path;
    switch (l.rot) {
        case 90:
        case 270: {
            const double edge = l.rot == 90 ? box.top() : box.bottom();
            path.moveTo(a);
            path.lineTo(a.x(), edge);
            path.addRoundedRect(box, 0.25, 0.25);
            break;
        }
        default: {
            const double dir = l.rot == 180 ? -1 : 1;
            const double far = dir > 0 ? box.right() : box.left();
            path.moveTo(a);
            path.lineTo(a.x() + dir * 0.5, box.top());
            path.lineTo(far - (l.port ? dir * 0.5 : 0), box.top());
            if (l.port) path.lineTo(far, box.center().y());  // a port is pointed at both ends
            path.lineTo(far - (l.port ? dir * 0.5 : 0), box.bottom());
            path.lineTo(a.x() + dir * 0.5, box.bottom());
            path.closeSubpath();
            break;
        }
    }
    if (l.port && (l.rot == 90 || l.rot == 270)) {
        // upright: a stem and a box pointed left and right
        const double edge = l.rot == 90 ? box.top() : box.bottom();
        path = QPainterPath();
        path.moveTo(a);
        path.lineTo(a.x(), edge);
        path.moveTo(box.left() + 0.4, box.top());
        path.lineTo(box.right() - 0.4, box.top());
        path.lineTo(box.right(), box.center().y());
        path.lineTo(box.right() - 0.4, box.bottom());
        path.lineTo(box.left() + 0.4, box.bottom());
        path.lineTo(box.left(), box.center().y());
        path.closeSubpath();
    }
    p.save();
    p.setTransform(viewTransform());
    QPen pen(ink, penWidth() / scale_);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    QColor fill = Theme::surface();
    if (l.port) {
        fill = accent();
        fill.setAlpha(Theme::dark() ? 60 : 35);
    }
    p.setBrush(fill);
    p.drawPath(path);
    p.restore();
    const QPointF centre = box.center();
    paintText(p, toScreen(centre), l.name, Qt::AlignCenter, kLabelFont, ink);
    if (!extra.isEmpty()) {
        const QPointF at = l.rot == 180 ? QPointF(box.left() - 0.3, centre.y())
                         : l.rot == 90 ? QPointF(centre.x(), box.bottom() + 0.2)
                         : l.rot == 270 ? QPointF(centre.x(), box.top() - 0.2)
                                        : QPointF(box.right() + 0.3, centre.y());
        const int align = l.rot == 180 ? Qt::AlignRight | Qt::AlignVCenter
                        : l.rot == 90 ? Qt::AlignHCenter | Qt::AlignTop
                        : l.rot == 270 ? Qt::AlignHCenter | Qt::AlignBottom
                                       : Qt::AlignLeft | Qt::AlignVCenter;
        paintText(p, toScreen(at), extra, align, kTextFont, accent());
    }
}

void SchematicView::paintGround(QPainter& p, const SGround& g, const QColor& ink) {
    Placement place;
    place.at = g.at;
    place.rot = g.rot;
    p.save();
    p.setTransform(place.transform() * viewTransform());
    QPen pen(ink, penWidth() / scale_);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    drawGround(p);
    p.restore();
}

void SchematicView::paintModel(QPainter& p, const Schematic& s, const Connectivity& c, bool) {
    const QColor ink = Theme::textPrimary(), sel = accent();
    const double w = penWidth();

    // the net under the mouse, underlaid
    if (!hoverNet_.isEmpty() && mode_ == Mode::Select && drag_ == Drag::None) {
        QColor glow = sel;
        glow.setAlpha(70);
        QPen pen(glow, w + std::max(4.0, scale_ * 0.35));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        for (int k = 0; k < s.wires.size(); ++k) {
            if (c.netNames.value(c.wireNet[k]) != hoverNet_) continue;
            QPolygonF poly;
            for (const QPoint& pt : s.wires[k].pts) poly << toScreen(pt);
            p.drawPolyline(poly);
        }
    }
    // wires
    for (int k = 0; k < s.wires.size(); ++k) {
        QPen pen(sel_.wires.contains(k) ? sel : ink, w);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        QPolygonF poly;
        for (const QPoint& pt : s.wires[k].pts) poly << toScreen(pt);
        p.drawPolyline(poly);
    }
    // dots and open ends
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    const double r = std::max(2.5, 0.28 * scale_);
    for (const QPoint& j : c.junctions) p.drawEllipse(toScreen(j), r, r);
    p.setBrush(Qt::NoBrush);
    QColor open = Theme::textSecondary();
    p.setPen(QPen(open, 1.0));
    for (const QPoint& o : c.openEnds) {
        const QPointF q = toScreen(o);
        p.drawRect(QRectF(q.x() - r, q.y() - r, 2 * r, 2 * r));
    }
    // components
    QHash<int, QVector<int>> conflicts;
    for (const auto& [comp, pin] : c.conflicts) conflicts[comp] << pin;
    for (int k = 0; k < s.components.size(); ++k) {
        const SComponent& comp = s.components[k];
        const bool on = sel_.comps.contains(comp.name);
        const bool hovered = hover_.type == ItemType::Component && hover_.index == k && mode_ == Mode::Select;
        paintComponent(p, comp, on ? sel : hovered ? mix(ink, sel, 0.55) : ink, false, conflicts.value(k));
    }
    // labels and grounds
    for (int k = 0; k < s.labels.size(); ++k) {
        const QString v = QStringLiteral("v(%1)").arg(s.labels[k].name);
        const QString extra = op_.contains(v) ? formatSI(op_.value(v), "V", 3) : QString();
        paintLabel(p, s.labels[k], sel_.labels.contains(k) ? sel : ink, extra);
    }
    for (int k = 0; k < s.grounds.size(); ++k) paintGround(p, s.grounds[k], sel_.grounds.contains(k) ? sel : ink);
}

void SchematicView::paintEvent(QPaintEvent*) {
    if (!fitted_ && width() > 50 && height() > 50) {
        fitted_ = true;
        fitAll();
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(rect(), Theme::surface());
    paintGrid(p);
    paintModel(p, display_, displayConn_, false);

    // what is being placed or drawn
    QColor ghost = accent();
    ghost.setAlpha(170);
    if (mode_ == Mode::Place && mouseInside_) {
        if (ghostKind_ == Ghost::Part) {
            SComponent c = ghost_;
            c.place.at = mouseGrid_;
            paintComponent(p, c, ghost, false, {});
        } else if (ghostKind_ == Ghost::Ground) {
            SGround g = ghostGround_;
            g.at = mouseGrid_;
            paintGround(p, g, ghost);
        } else {
            SLabel l = ghostLabel_;
            l.at = mouseGrid_;
            paintLabel(p, l, ghost, QString());
        }
    }
    if (mode_ == Mode::Wire) {
        QPolygonF poly;
        for (const QPoint& pt : wirePts_) poly << toScreen(pt);
        if (!wirePts_.isEmpty() && mouseInside_)
            for (const QPoint& pt : route(wirePts_.last(), mouseGrid_)) poly << toScreen(pt);
        QPen pen(ghost, penWidth());
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPolyline(poly);
        if (mouseInside_) {
            // cross hair at the snapped point
            const QPointF m = toScreen(mouseGrid_);
            const double a = std::max(6.0, scale_ * 0.6);
            p.setPen(QPen(ghost, 1.0));
            p.drawLine(m - QPointF(a, 0), m + QPointF(a, 0));
            p.drawLine(m - QPointF(0, a), m + QPointF(0, a));
        }
    }
    if (drag_ == Drag::Rubber && !rubber_.isEmpty()) {
        QColor fill = accent();
        fill.setAlpha(30);
        p.setPen(QPen(accent(), 1.0));
        p.setBrush(fill);
        p.drawRect(rubber_);
    }

    // hints at the bottom, a banner while the text is invalid
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.9);
    p.setFont(small);
    const QFontMetricsF fm(small);
    QString hint;
    if (mode_ == Mode::Place)
        hint = tr("Klick: platzieren · R drehen · X spiegeln · Esc: fertig");
    else if (mode_ == Mode::Wire)
        hint = tr("Klick: Ecke · endet an Anschluss oder Leitung · Doppelklick/Esc: beenden · %1: letzte Ecke · /: Knick")
                   .arg(Keys::erase());
    else if (hasFocus())
        hint = tr("R drehen · X spiegeln · %1 löschen · %2 verdoppeln · W Leitung · G Masse · N Label · %3-Klick: Kurve")
                   .arg(Keys::erase(), Keys::cmd("D"), Keys::alt());
    if (!hint.isEmpty()) {
        p.setPen(Theme::textSecondary());
        p.drawText(QRectF(10, height() - fm.height() - 8, width() - 20, fm.height() + 4), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(hint, Qt::ElideRight, width() - 20));
    }
    if (model_.components.isEmpty() && model_.wires.isEmpty() && mode_ == Mode::Select) {
        p.setPen(Theme::textSecondary());
        p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("Bauteile aus der Liste „Bauteile“ wählen und hier platzieren.\nW: Leitung · G: Masse · N: Netzlabel"));
    }
    if (stale_) {
        const QString text = tr("Der Text ist zurzeit kein gültiges JSON – der Plan zeigt den letzten gültigen Stand.");
        const QRectF bar(0, 0, width(), fm.height() + 12);
        QColor bg = Theme::series(3);
        bg.setAlpha(Theme::dark() ? 90 : 60);
        p.fillRect(bar, bg);
        p.setPen(Theme::textPrimary());
        p.drawText(bar.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(text, Qt::ElideRight, width() - 20));
    }
    if (hasFocus()) {
        QColor frame = accent();
        frame.setAlpha(110);
        p.setPen(QPen(frame, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    }
}
