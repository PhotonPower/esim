#include "Schematic.hpp"

#include <QRegularExpression>
#include <algorithm>
#include <climits>
#include <cmath>
#include <numeric>

#include "JsonSpan.hpp"
#include "Symbols.hpp"

// --- placement ------------------------------------------------------------------

QPoint Placement::mapDir(QPoint d) const {
    if (mirror) d.setX(-d.x());
    switch (((rot % 360) + 360) % 360) {
        case 90: return QPoint(-d.y(), d.x());
        case 180: return QPoint(-d.x(), -d.y());
        case 270: return QPoint(d.y(), -d.x());
        default: return d;
    }
}

QPoint Placement::map(QPoint local) const { return mapDir(local) + at; }

QPointF Placement::map(QPointF local) const { return transform().map(local); }

QTransform Placement::transform() const {
    QTransform t;
    t.translate(at.x(), at.y());
    t.rotate(rot);  // clockwise on the screen (y down)
    if (mirror) t.scale(-1, 1);
    return t;
}

bool isGroundName(const QString& node) {
    return node == "0" || node == "gnd" || node == "GND" || node == "Gnd";
}

namespace {

bool sameNode(const QString& a, const QString& b) { return a == b || (isGroundName(a) && isGroundName(b)); }

// grows r to contain p (QRectF::united ignores empty rectangles)
void extend(QRectF& r, QPointF p, bool& any) {
    if (!any) {
        r = QRectF(p, QSizeF(0, 0));
        any = true;
        return;
    }
    r.setLeft(std::min(r.left(), p.x()));
    r.setRight(std::max(r.right(), p.x()));
    r.setTop(std::min(r.top(), p.y()));
    r.setBottom(std::max(r.bottom(), p.y()));
}

void extend(QRectF& r, const QRectF& b, bool& any) {
    extend(r, b.topLeft(), any);
    extend(r, b.bottomRight(), any);
}

QString defaultName(char kind, int index) {
    return QString(QChar(kind ? kind : 'u').toUpper()) + QString::number(index + 1);
}

// the object holding "components" and "schematic": the file, or a subcircuit
const JValue* scopeOf(const JValue& root, const QString& scope) {
    if (!root.isObject()) return nullptr;
    if (scope.isEmpty()) return &root;
    const JValue* subs = root.get("subcircuits");
    const JValue* def = subs && subs->isObject() ? subs->get(scope) : nullptr;
    return def && def->isObject() ? def : nullptr;
}

const JValue* componentsOf(const JValue& root, const QString& scope) {
    const JValue* obj = scopeOf(root, scope);
    const JValue* comps = obj ? obj->get("components") : nullptr;
    return comps && comps->isArray() ? comps : nullptr;
}

QString memberIndent(const QString& text, const JValue& obj) {
    if (obj.keyBegins.empty()) return indentationAt(text, obj.begin) + QStringLiteral("  ");
    return indentationAt(text, obj.keyBegins.front());
}


char kindOf(const QString& type) {
    static const QHash<QString, char> kinds = {
        {"r", 'r'}, {"resistor", 'r'}, {"c", 'c'}, {"capacitor", 'c'}, {"l", 'l'}, {"inductor", 'l'},
        {"v", 'v'}, {"vsource", 'v'}, {"i", 'i'}, {"isource", 'i'}, {"e", 'e'}, {"vcvs", 'e'},
        {"g", 'g'}, {"vccs", 'g'}, {"f", 'f'}, {"cccs", 'f'}, {"h", 'h'}, {"ccvs", 'h'},
        {"d", 'd'}, {"diode", 'd'}, {"q", 'q'}, {"bjt", 'q'}, {"npn", 'q'}, {"pnp", 'q'},
        {"m", 'm'}, {"mosfet", 'm'}, {"nmos", 'm'}, {"pmos", 'm'}, {"b", 'b'}, {"behavioral", 'b'},
        {"s", 's'}, {"switch", 's'}, {"ideal_diode", 'y'}, {"j", 'j'}, {"jfet", 'j'}, {"njf", 'j'},
        {"pjf", 'j'}, {"k", 'k'}, {"coupling", 'k'}, {"t", 't'}, {"tline", 't'}, {"x", 'x'},
        {"subcircuit", 'x'}};
    return kinds.value(type.toLower(), 0);
}

QString shorten(const QString& s, int n = 22) { return s.size() <= n ? s : s.left(n - 1) + QString::fromUtf8("…"); }

// the value of a member as short text, "" if missing or not a scalar
QString member(const QString& text, const JValue& obj, const char* key) {
    const JValue* v = obj.get(QString::fromLatin1(key));
    return v ? v->text(text) : QString();
}

QString firstMember(const QString& text, const JValue& obj, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const QString v = member(text, obj, k);
        if (!v.isEmpty()) return v;
    }
    return QString();
}

QString sourceSummary(const QString& text, const JValue& obj, QString& variant) {
    QStringList parts;
    if (const JValue* w = obj.get("wave"); w && w->isObject()) {
        const QString type = member(text, *w, "type").toLower();
        variant = type == "sine" ? QStringLiteral("sin") : type;
        if (variant == "sin") {
            parts << QStringLiteral("sin %1").arg(firstMember(text, *w, {"amplitude", "va", "ampl"}));
            const QString f = firstMember(text, *w, {"freq", "frequency", "f"});
            if (!f.isEmpty()) parts.last() += QStringLiteral(", %1 Hz").arg(f);
        } else if (variant == "pulse") {
            parts << QStringLiteral("pulse %1 … %2").arg(member(text, *w, "v1"), member(text, *w, "v2"));
        } else {
            parts << variant;
        }
    }
    const QString dc = firstMember(text, obj, {"dc", "value"});
    if (!dc.isEmpty() && (parts.isEmpty() || dc != "0")) parts.prepend(dc);
    const QString ac = member(text, obj, "ac");
    if (!ac.isEmpty()) parts << QStringLiteral("ac %1").arg(ac);
    return parts.join(QStringLiteral(" · "));
}

QPoint pointOf(const JValue& v, bool* ok) {
    *ok = v.isArray() && v.items.size() == 2 && v.items[0].isNumber() && v.items[1].isNumber();
    return *ok ? QPoint(qRound(v.items[0].number), qRound(v.items[1].number)) : QPoint();
}

int rotOf(const JValue& obj) {
    const JValue* r = obj.get("rot");
    if (!r || !r->isNumber()) return 0;
    const int rot = (int(std::lround(r->number / 90.0)) * 90 % 360 + 360) % 360;
    return rot;
}

// rotation of a ground symbol / label that points along d
int groundRot(QPoint d) { return d.y() > 0 ? 0 : d.x() < 0 ? 90 : d.y() < 0 ? 180 : 270; }
int labelRot(QPoint d) { return d.x() > 0 ? 0 : d.y() > 0 ? 90 : d.x() < 0 ? 180 : 270; }

// Places components without a position below everything placed, in rows,
// with a label (or ground symbol) at every pin naming its node.
void autoPlace(Schematic& s) {
    QVector<int> todo;
    for (int k = 0; k < s.components.size(); ++k)
        if (!s.components[k].placed) todo << k;
    if (todo.isEmpty()) return;
    const auto labelWidth = [](const QString& n) { return int(std::ceil(n.size() * 0.62 + 1.2)); };

    // the cell of each component: symbol, labels around it, name and value on the right
    struct Cell {
        int w, h;
        QPoint origin;  // of the symbol, relative to the cell
    };
    QVector<Cell> cells;
    double area = 0;
    for (int k : todo) {
        SComponent& c = s.components[k];
        c.place = Placement();
        const Symbol sym = symbolOf(c);
        QRectF box = sym.body;
        bool anyBox = true;
        int extL = 0, extR = 0, extT = 0, extB = 0;
        for (int p = 0; p < sym.pins.size(); ++p) {
            const SymPin& pin = sym.pins[p];
            extend(box, QPointF(pin.pos), anyBox);
            const QString node = p < c.nodes.size() ? c.nodes[p] : QString();
            if (node.isEmpty()) continue;
            const int w = isGroundName(node) ? 2 : labelWidth(node);
            if (pin.dir.x() < 0) extL = std::max(extL, w);
            if (pin.dir.x() > 0) extR = std::max(extR, w);
            if (pin.dir.y() < 0) extT = std::max(extT, 2);
            if (pin.dir.y() > 0) extB = std::max(extB, 2);
        }
        const int textW = int(std::ceil(std::max(c.name.size(), c.summary.size()) * 0.5)) + 1;
        // name and value go above symbols whose first pin points sideways (see SchematicView)
        const bool textAbove = !sym.pins.isEmpty() && sym.pins.first().dir.x() != 0;
        if (textAbove) extT += 2;
        Cell cell;
        cell.w = extL + int(std::ceil(box.width())) + (textAbove ? extR : std::max(extR, textW)) + 3;
        cell.h = extT + int(std::ceil(box.height())) + extB + 2;
        cell.origin = QPoint(extL + int(std::ceil(-box.left())) + 1, extT + int(std::ceil(-box.top())) + 1);
        cells << cell;
        area += double(cell.w) * cell.h;
    }
    // rows about half again as wide as high, like the drawing area
    const int rowWidth = std::clamp(int(std::sqrt(area) * 1.5), 20, 90);
    const QRect used = s.bounds(true);
    const int left = used.isValid() ? used.left() : 0;
    int y = used.isValid() ? used.bottom() + 5 : 0;
    int x = left, rowHeight = 0;
    for (int i = 0; i < todo.size(); ++i) {
        SComponent& c = s.components[todo[i]];
        const Cell& cell = cells[i];
        if (x > left && x + cell.w > left + rowWidth) {
            x = left;
            y += rowHeight;
            rowHeight = 0;
        }
        c.place.at = QPoint(x, y) + cell.origin;
        x += cell.w;
        rowHeight = std::max(rowHeight, cell.h);
        const Symbol sym = symbolOf(c);
        for (int p = 0; p < sym.pins.size() && p < c.nodes.size(); ++p) {
            const QString node = c.nodes[p];
            if (node.isEmpty()) continue;
            const QPoint at = c.place.map(sym.pins[p].pos), dir = c.place.mapDir(sym.pins[p].dir);
            if (isGroundName(node)) s.grounds.push_back({at, groundRot(dir)});
            else s.labels.push_back({node, at, labelRot(dir), s.ports.contains(node)});
        }
    }
}

// Ports of a subcircuit that the drawing does not show yet, in a column on the left.
void placePorts(Schematic& s) {
    QStringList missing = s.ports;
    for (const SLabel& l : s.labels)
        if (l.port) missing.removeAll(l.name);
    if (missing.isEmpty()) return;
    const QRect used = s.bounds();
    const int x = used.isValid() ? used.left() - 8 : 0;
    int y = used.isValid() ? used.top() : 0;
    for (const QString& p : missing) {
        s.labels.push_back({p, QPoint(x, y), 0, true});
        y += 3;
    }
}

}  // namespace

// --- the model ------------------------------------------------------------------

int Schematic::find(const QString& name) const {
    for (int k = 0; k < components.size(); ++k)
        if (components[k].name.compare(name, Qt::CaseInsensitive) == 0) return k;
    return -1;
}

QString Schematic::freeName(const QString& prefix) const {
    for (int k = 1;; ++k) {
        const QString name = prefix + QString::number(k);
        if (!nameTaken(name)) return name;
    }
}

QRect Schematic::bounds(bool placedOnly) const {
    QRectF r;
    bool any = false;
    for (const SComponent& c : components) {
        if (placedOnly && !c.placed) continue;
        const Symbol sym = symbolOf(c);
        extend(r, c.place.transform().mapRect(sym.body), any);
        for (const SymPin& p : sym.pins) extend(r, QPointF(c.place.map(p.pos)), any);
    }
    for (const SWire& w : wires)
        for (const QPoint& p : w.pts) extend(r, QPointF(p), any);
    for (const SLabel& l : labels) extend(r, QPointF(l.at), any);
    for (const SGround& g : grounds) extend(r, QPointF(g.at), any);
    if (!any) return QRect();
    return QRect(QPoint(int(std::floor(r.left())), int(std::floor(r.top()))),
                 QPoint(int(std::ceil(r.right())), int(std::ceil(r.bottom()))));
}

bool readSchematic(const QString& text, Schematic& out, QString* error, const QString& scope) {
    JValue root;
    if (!parseJsonc(text, root, error)) return false;
    if (!root.isObject()) {
        if (error) *error = QStringLiteral("the file is no JSON object");
        return false;
    }
    const JValue* home = scopeOf(root, scope);
    if (!home) {
        if (error) *error = QStringLiteral("no subcircuit \"%1\"").arg(scope);
        return false;
    }
    Schematic s;
    s.scope = scope;
    if (!scope.isEmpty())
        if (const JValue* p = home->get("ports"); p && p->isArray())
            for (const JValue& v : p->items) s.ports << v.text(text);
    for (const QString& p : s.ports) s.textNodes.insert(p);

    if (const JValue* models = root.get("models"); models && models->isObject()) {
        static const QRegularExpression card(QStringLiteral(R"(^\s*\.model\s+\S+\s+([A-Za-z]+))"),
                                             QRegularExpression::CaseInsensitiveOption);
        for (size_t k = 0; k < models->items.size(); ++k) {
            const JValue& m = models->items[k];
            QString type;
            if (m.isObject()) type = member(text, m, "type");
            else if (m.isString()) type = card.match(m.string).captured(1);
            s.modelTypes.insert(models->keys[int(k)], type.toLower());
        }
    }
    if (const JValue* subs = root.get("subcircuits"); subs && subs->isObject()) {
        for (size_t k = 0; k < subs->items.size(); ++k) {
            const JValue& def = subs->items[k];
            const QString name = subs->keys[int(k)];
            QStringList ports;
            if (const JValue* p = def.get("ports"); p && p->isArray()) {
                for (const JValue& v : p->items) ports << v.text(text);
            } else if (def.isString() || def.isArray()) {
                // SPICE: the ports of ".subckt NAME port ..." (that name, else the first)
                QStringList lines;
                if (def.isString()) lines = def.string.split('\n');
                for (const JValue& l : def.items) lines << l.string;
                QStringList first;
                for (const QString& l : lines) {
                    const QStringList w = l.simplified().split(' ');
                    if (w.value(0).compare(".subckt", Qt::CaseInsensitive) != 0 || w.size() < 2) continue;
                    QStringList p;
                    for (int i = 2; i < w.size() && !w[i].contains('=') && w[i].compare("params:", Qt::CaseInsensitive) != 0; ++i)
                        p << w[i].toLower();
                    if (first.isEmpty()) first = p;
                    if (w[1].compare(name, Qt::CaseInsensitive) == 0) {
                        first = p;
                        break;
                    }
                }
                ports = first;
                s.spiceSubcircuits.insert(name);
            }
            s.subcircuits.insert(name, ports);
        }
    }

    // the drawing
    QHash<QString, Placement> placements;
    if (const JValue* sec = home->get("schematic"); sec && sec->isObject()) {
        s.hasSection = true;
        bool ok;
        if (const JValue* comps = sec->get("components"); comps && comps->isObject()) {
            for (size_t k = 0; k < comps->items.size(); ++k) {
                const JValue& e = comps->items[k];
                const JValue* at = e.get("at");
                if (!at) continue;
                Placement p;
                p.at = pointOf(*at, &ok);
                if (!ok) continue;
                p.rot = rotOf(e);
                const JValue* m = e.get("mirror");
                p.mirror = m && m->type == JValue::Bool && m->boolean;
                placements.insert(comps->keys[int(k)], p);
            }
        }
        if (const JValue* wires = sec->get("wires"); wires && wires->isArray()) {
            for (const JValue& w : wires->items) {
                SWire wire;
                for (const JValue& p : w.items) {
                    const QPoint pt = pointOf(p, &ok);
                    if (ok && (wire.pts.isEmpty() || wire.pts.last() != pt)) wire.pts << pt;
                }
                if (wire.pts.size() >= 2) s.wires << wire;
            }
        }
        for (const char* key : {"ports", "labels"}) {
            const JValue* labels = sec->get(QString::fromLatin1(key));
            if (!labels || !labels->isArray() || (key[0] == 'p' && scope.isEmpty())) continue;
            for (const JValue& l : labels->items) {
                const JValue* at = l.get("at");
                const QString name = member(text, l, "name");
                if (!at || name.isEmpty()) continue;
                const QPoint p = pointOf(*at, &ok);
                if (ok) s.labels.push_back({name, p, rotOf(l), key[0] == 'p'});
            }
        }
        if (const JValue* grounds = sec->get("grounds"); grounds && grounds->isArray()) {
            for (const JValue& g : grounds->items) {
                const JValue* at = g.isObject() ? g.get("at") : &g;
                if (!at) continue;
                const QPoint p = pointOf(*at, &ok);
                if (ok) s.grounds.push_back({p, g.isObject() ? rotOf(g) : 0});
            }
        }
    }

    // the components
    if (const JValue* comps = home->get("components"); comps && comps->isArray()) {
        for (size_t i = 0; i < comps->items.size(); ++i) {
            const JValue& o = comps->items[i];
            if (!o.isObject()) continue;
            SComponent c;
            c.index = int(i);
            c.begin = o.begin;
            c.end = o.end;
            c.type = member(text, o, "type");
            c.kind = kindOf(c.type);
            c.name = member(text, o, "name");
            if (c.name.isEmpty()) c.name = defaultName(c.kind, int(i));
            if (const JValue* n = o.get("nodes"); n && n->isArray())
                for (const JValue& v : n->items) c.nodes << v.text(text);
            const QString type = c.type.toLower();
            const QString model = member(text, o, "model");
            c.model = model;
            const QString modelType = s.modelTypes.value(model);
            const auto polarity = [&](const char* a, const char* b) {
                const QString pa = QString::fromLatin1(a), pb = QString::fromLatin1(b);
                c.variant = type == pb || modelType == pb ? pb : pa;
            };
            switch (c.kind) {
                case 'r': c.summary = firstMember(text, o, {"value", "r"}); break;
                case 'c': c.summary = firstMember(text, o, {"value", "c"}); break;
                case 'l': c.summary = firstMember(text, o, {"value", "l"}); break;
                case 'v':
                case 'i': c.summary = sourceSummary(text, o, c.variant); break;
                case 'q': polarity("npn", "pnp"); c.summary = model; break;
                case 'm': polarity("nmos", "pmos"); c.summary = model; break;
                case 'j': polarity("njf", "pjf"); c.summary = model; break;
                case 'd': c.summary = model; break;
                case 'y': {
                    const QString vf = member(text, o, "vf");
                    c.summary = vf.isEmpty() ? QString() : QStringLiteral("vf %1").arg(vf);
                    break;
                }
                case 'e': c.summary = QString::fromUtf8("× %1").arg(member(text, o, "gain")); break;
                case 'g': c.summary = QStringLiteral("gm %1").arg(firstMember(text, o, {"gm", "gain"})); break;
                case 'f':
                    c.summary = QString::fromUtf8("%1 × i(%2)").arg(member(text, o, "gain"), member(text, o, "control"));
                    break;
                case 'h':
                    c.summary = QString::fromUtf8("%1 × i(%2)").arg(firstMember(text, o, {"r", "gain"}),
                                                                     member(text, o, "control"));
                    break;
                case 'b':
                    for (const char* k : {"i", "v", "q", "phi"}) {
                        const QString e = member(text, o, k);
                        if (e.isEmpty()) continue;
                        c.variant = QString::fromLatin1(k);
                        c.summary = shorten(QStringLiteral("%1 = %2").arg(c.variant, e));
                        break;
                    }
                    break;
                case 's': {
                    const QString ctrl = member(text, o, "control");
                    c.summary = shorten(ctrl.isEmpty() ? QStringLiteral("vt %1").arg(firstMember(text, o, {"vt", "threshold"}))
                                                       : ctrl);
                    break;
                }
                case 't': {
                    const QString z0 = member(text, o, "z0"), td = member(text, o, "td");
                    c.summary = z0.isEmpty() ? QString() : QString::fromUtf8("%1 Ω, %2").arg(z0, td);
                    break;
                }
                case 'x':
                    c.subcircuit = member(text, o, "subcircuit");
                    c.ports = s.subcircuits.value(c.subcircuit);
                    c.summary = c.subcircuit;
                    break;
                case 'k':
                    if (const JValue* l = o.get("inductors"); l && l->isArray())
                        for (const JValue& v : l->items) c.ports << v.text(text);
                    c.summary = QString::fromUtf8("%1 ↔ %2, k = %3")
                                    .arg(c.ports.value(0), c.ports.value(1), member(text, o, "k"));
                    break;
                default: c.summary = c.type; break;
            }
            // the other members as written, for a copy of the component
            QStringList raw;
            for (size_t m = 0; m < o.items.size(); ++m) {
                const QString& key = o.keys[int(m)];
                if (key == "type" || key == "name" || key == "nodes" || key == "subcircuit") continue;
                raw << text.mid(o.keyBegins[m], o.items[m].end - o.keyBegins[m]);
            }
            c.extra = raw.join(QStringLiteral(", "));
            for (const QString& n : c.nodes) s.textNodes.insert(n);
            auto it = placements.find(c.name);
            if (it != placements.end()) {
                c.place = *it;
                c.placed = true;
            }
            s.components << c;
        }
    }
    autoPlace(s);
    placePorts(s);
    out = std::move(s);
    return true;
}

// --- connections ----------------------------------------------------------------

namespace {

struct UnionFind {
    QVector<int> parent;
    int add() {
        parent << int(parent.size());
        return int(parent.size()) - 1;
    }
    int find(int a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    }
    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
};

bool onSegment(QPoint p, QPoint a, QPoint b) {
    if (a.x() == b.x()) return p.x() == a.x() && p.y() >= std::min(a.y(), b.y()) && p.y() <= std::max(a.y(), b.y());
    if (a.y() == b.y()) return p.y() == a.y() && p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x());
    // a slanted segment (not drawn by the editor, but possible in a file)
    const qint64 cross = qint64(b.x() - a.x()) * (p.y() - a.y()) - qint64(b.y() - a.y()) * (p.x() - a.x());
    return cross == 0 && p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x()) &&
           p.y() >= std::min(a.y(), b.y()) && p.y() <= std::max(a.y(), b.y());
}

}  // namespace

Connectivity connectSchematic(const Schematic& s) {
    Connectivity c;
    UnionFind uf;
    QHash<QPoint, int> ids;
    const auto id = [&](QPoint p) {
        auto it = ids.find(p);
        if (it != ids.end()) return *it;
        const int n = uf.add();
        ids.insert(p, n);
        return n;
    };

    // pins
    c.pinPos.resize(s.components.size());
    for (int k = 0; k < s.components.size(); ++k) {
        const SComponent& comp = s.components[k];
        for (const SymPin& p : symbolOf(comp).pins) c.pinPos[k] << comp.place.map(p.pos);
    }
    // wires: all corners of a wire are one conductor
    struct Segment { QPoint a, b; };
    QVector<Segment> segments;
    for (const SWire& w : s.wires) {
        for (int k = 0; k + 1 < w.pts.size(); ++k) {
            uf.unite(id(w.pts[k]), id(w.pts[k + 1]));
            segments.push_back({w.pts[k], w.pts[k + 1]});
        }
    }
    // everything that can touch a wire
    QVector<QPoint> points;
    for (const auto& pins : c.pinPos) points += pins;
    for (const SWire& w : s.wires) points += w.pts;
    for (const SLabel& l : s.labels) points << l.at;
    for (const SGround& g : s.grounds) points << g.at;
    QHash<QPoint, int> degree;
    for (const QPoint& p : points) id(p);
    {
        QSet<QPoint> seen;
        for (const QPoint& p : points) {
            if (seen.contains(p)) continue;
            seen.insert(p);
            for (const Segment& seg : segments) {
                if (!onSegment(p, seg.a, seg.b)) continue;
                uf.unite(id(p), id(seg.a));
                if (p != seg.a && p != seg.b) degree[p] += 2;  // a wire passes through
            }
        }
    }
    // labels with the same name are one net
    QHash<QString, int> byName;
    for (const SLabel& l : s.labels) {
        const QString key = isGroundName(l.name) ? QStringLiteral("0") : l.name;
        auto it = byName.find(key);
        if (it == byName.end()) byName.insert(key, id(l.at));
        else uf.unite(*it, id(l.at));
    }
    for (const SGround& g : s.grounds) {
        auto it = byName.find(QStringLiteral("0"));
        if (it == byName.end()) byName.insert(QStringLiteral("0"), id(g.at));
        else uf.unite(*it, id(g.at));
    }

    // net numbers in the order of appearance
    QHash<int, int> netOf;
    const auto net = [&](QPoint p) {
        const int root = uf.find(id(p));
        auto it = netOf.find(root);
        if (it != netOf.end()) return *it;
        const int n = int(netOf.size());
        netOf.insert(root, n);
        return n;
    };
    c.pinNet.resize(s.components.size());
    for (int k = 0; k < s.components.size(); ++k)
        for (const QPoint& p : c.pinPos[k]) c.pinNet[k] << net(p);
    for (const SLabel& l : s.labels) c.labelNet << net(l.at);
    for (const SGround& g : s.grounds) c.groundNet << net(g.at);
    for (const SWire& w : s.wires) c.wireNet << net(w.pts.first());
    const int nets = int(netOf.size());

    // names: ground, labels, the name the pins had, else new
    c.netNames = QStringList(QVector<QString>(nets));
    QVector<QStringList> labelNames(nets);
    QVector<bool> grounded(nets, false);
    for (int k = 0; k < s.labels.size(); ++k) labelNames[c.labelNet[k]] << s.labels[k].name;
    for (int k = 0; k < s.grounds.size(); ++k) grounded[c.groundNet[k]] = true;
    QSet<QString> taken;
    for (int n = 0; n < nets; ++n) {
        QStringList names = labelNames[n];
        names.removeDuplicates();
        bool ground = grounded[n];
        for (const QString& l : names) ground = ground || isGroundName(l);
        if (ground) {
            c.netNames[n] = QStringLiteral("0");
            for (const QString& l : names)
                if (!isGroundName(l))
                    c.warnings << QString::fromUtf8("Das Label „%1“ liegt an Masse").arg(l);
        } else if (!names.isEmpty()) {
            names.sort();
            c.netNames[n] = names.first();
            if (names.size() > 1)
                c.warnings << QString::fromUtf8("Ein Netz trägt mehrere Labels: %1").arg(names.join(", "));
        }
        if (!c.netNames[n].isEmpty()) taken.insert(c.netNames[n]);
    }
    // pins per net, in order
    QVector<QVector<QPair<int, int>>> pinsOf(nets);
    for (int k = 0; k < s.components.size(); ++k)
        for (int p = 0; p < c.pinNet[k].size(); ++p) pinsOf[c.pinNet[k][p]].push_back({k, p});
    QVector<int> order(nets);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return pinsOf[a].size() > pinsOf[b].size(); });
    QSet<QString> used = s.textNodes;
    for (const SLabel& l : s.labels) used.insert(l.name);
    int fresh = 1;
    for (int n : order) {
        if (!c.netNames[n].isEmpty() || pinsOf[n].isEmpty()) continue;
        // the most frequent earlier name that is still free (ground only by symbol)
        QStringList candidates;
        QHash<QString, int> count;
        for (const auto& [k, p] : pinsOf[n]) {
            const QString old = s.components[k].nodes.value(p);
            if (old.isEmpty() || isGroundName(old)) continue;
            if (!count.contains(old)) candidates << old;
            count[old]++;
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&](const QString& a, const QString& b) { return count[a] > count[b]; });
        for (const QString& name : candidates) {
            if (taken.contains(name)) continue;
            c.netNames[n] = name;
            break;
        }
        if (c.netNames[n].isEmpty()) {
            QString name;
            do name = QStringLiteral("n%1").arg(fresh++);
            while (used.contains(name) || taken.contains(name));
            c.netNames[n] = name;
        }
        taken.insert(c.netNames[n]);
    }

    c.pinNodes.resize(s.components.size());
    for (int k = 0; k < s.components.size(); ++k) {
        for (int p = 0; p < c.pinNet[k].size(); ++p) c.pinNodes[k] << c.netNames[c.pinNet[k][p]];
        const SComponent& comp = s.components[k];
        if (comp.index < 0 || comp.nodes.size() != c.pinNodes[k].size()) continue;
        for (int p = 0; p < comp.nodes.size(); ++p)
            if (!sameNode(comp.nodes[p], c.pinNodes[k][p])) c.conflicts.push_back({k, p});
    }

    // dots where three or more connections meet, and open wire ends
    for (const SWire& w : s.wires) {
        degree[w.pts.first()] += 1;
        degree[w.pts.last()] += 1;
    }
    QSet<QPoint> pinPoints, anchors;
    for (const auto& pins : c.pinPos)
        for (const QPoint& p : pins) {
            degree[p] += 1;
            pinPoints.insert(p);
        }
    for (const SLabel& l : s.labels) anchors.insert(l.at);
    for (const SGround& g : s.grounds) anchors.insert(g.at);
    for (auto it = degree.begin(); it != degree.end(); ++it)
        if (it.value() >= 3) c.junctions << it.key();
    for (const SWire& w : s.wires)
        for (const QPoint& end : {w.pts.first(), w.pts.last()})
            if (degree.value(end) == 1 && !anchors.contains(end) && !pinPoints.contains(end)) c.openEnds << end;
    return c;
}

// --- writing ----------------------------------------------------------------------

namespace {

QString pointJson(QPoint p) { return QStringLiteral("[%1, %2]").arg(p.x()).arg(p.y()); }

QString nodesJson(const QStringList& nodes) {
    QStringList q;
    for (const QString& n : nodes) q << jsonQuote(n);
    return QStringLiteral("[") + q.join(QStringLiteral(", ")) + QStringLiteral("]");
}

QString componentJson(const SComponent& c, const QStringList& nodes) {
    QString out = QStringLiteral("{ \"type\": %1, \"name\": %2").arg(jsonQuote(c.type), jsonQuote(c.name));
    if (c.kind == 'x') out += QStringLiteral(", \"subcircuit\": %1").arg(jsonQuote(c.subcircuit));
    if (c.kind != 'k') out += QStringLiteral(", \"nodes\": %1").arg(nodesJson(nodes));
    if (!c.extra.isEmpty()) out += QStringLiteral(", ") + c.extra;
    return out + QStringLiteral(" }");
}

}  // namespace

QString sectionJson(const Schematic& s, const QString& indent) {
    const QString in1 = indent + QStringLiteral("  "), in2 = in1 + QStringLiteral("  ");
    QStringList parts;
    QStringList comps;
    for (const SComponent& c : s.components) {
        QString e = QStringLiteral("%1: { \"at\": %2").arg(jsonQuote(c.name), pointJson(c.place.at));
        if (c.place.rot) e += QStringLiteral(", \"rot\": %1").arg(c.place.rot);
        if (c.place.mirror) e += QStringLiteral(", \"mirror\": true");
        comps << e + QStringLiteral(" }");
    }
    parts << (comps.isEmpty() ? in1 + QStringLiteral("\"components\": {}")
                              : in1 + QStringLiteral("\"components\": {\n") + in2 + comps.join(",\n" + in2) + "\n" +
                                    in1 + "}");
    const auto list = [&](const char* key, const QStringList& items) {
        if (items.isEmpty()) return;
        parts << in1 + QStringLiteral("\"%1\": [\n").arg(QString::fromLatin1(key)) + in2 + items.join(",\n" + in2) +
                     "\n" + in1 + "]";
    };
    QStringList wires;
    for (const SWire& w : s.wires) {
        QStringList pts;
        for (const QPoint& p : w.pts) pts << pointJson(p);
        wires << QStringLiteral("[") + pts.join(QStringLiteral(", ")) + QStringLiteral("]");
    }
    list("wires", wires);
    QStringList labels, ports;
    for (const SLabel& l : s.labels) {
        QString e = QStringLiteral("{ \"name\": %1, \"at\": %2").arg(jsonQuote(l.name), pointJson(l.at));
        if (l.rot) e += QStringLiteral(", \"rot\": %1").arg(l.rot);
        (l.port ? ports : labels) << e + QStringLiteral(" }");
    }
    list("ports", ports);
    list("labels", labels);
    QStringList grounds;
    for (const SGround& g : s.grounds) {
        QString e = QStringLiteral("{ \"at\": %1").arg(pointJson(g.at));
        if (g.rot) e += QStringLiteral(", \"rot\": %1").arg(g.rot);
        grounds << e + QStringLiteral(" }");
    }
    list("grounds", grounds);
    return QStringLiteral("{\n") + parts.join(QStringLiteral(",\n")) + QStringLiteral("\n") + indent +
           QStringLiteral("}");
}

QString writeSchematic(const QString& text, const Schematic& after) {
    JValue root;
    if (!parseJsonc(text, root) || !scopeOf(root, after.scope)) return text;
    const Connectivity conn = connectSchematic(after);
    QString out = text;
    const QString& scope = after.scope;

    // 1. names and nodes of the components that stay, from the last to the first
    QSet<int> kept;
    QVector<int> order;
    for (int k = 0; k < after.components.size(); ++k)
        if (after.components[k].index >= 0) order << k;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return after.components[a].index > after.components[b].index; });
    for (int k : order) {
        const SComponent& c = after.components[k];
        kept.insert(c.index);
        JValue now;
        parseJsonc(out, now);
        const JValue* comps = componentsOf(now, scope);
        if (!comps || c.index >= int(comps->items.size())) continue;
        const JValue& obj = comps->items[size_t(c.index)];
        if (!obj.isObject()) continue;
        QVector<QPair<QString, QString>> changes;
        const JValue* name = obj.get("name");
        const QString current = name ? name->text(out) : defaultName(c.kind, c.index);
        if (c.name != current) changes.push_back({QStringLiteral("name"), jsonQuote(c.name)});
        // nodes, if the symbol has one pin per node (a subcircuit may have gained ports)
        const int pins = int(conn.pinNodes[k].size());
        if (c.kind && c.kind != 'k' && (pins == c.nodes.size() || (c.kind == 'x' && pins > c.nodes.size()))) {
            QStringList nodes;
            for (int p = 0; p < pins; ++p)
                nodes << (p < c.nodes.size() && sameNode(c.nodes[p], conn.pinNodes[k][p]) ? c.nodes[p]
                                                                                          : conn.pinNodes[k][p]);
            if (nodes != c.nodes) changes.push_back({QStringLiteral("nodes"), nodesJson(nodes)});
        }
        // the member further back first, so that the positions of the others stay valid
        std::sort(changes.begin(), changes.end(), [&](const auto& a, const auto& b) {
            const int pa = obj.indexOf(a.first) < 0 ? INT_MAX : obj.get(a.first)->begin;
            const int pb = obj.indexOf(b.first) < 0 ? INT_MAX : obj.get(b.first)->begin;
            return pa > pb;
        });
        for (const auto& [key, json] : changes) out = setMember(out, obj, key, json);
    }

    // 2. the drawing
    {
        JValue now;
        parseJsonc(out, now);
        const JValue* obj = scopeOf(now, scope);
        const QString section = sectionJson(after, memberIndent(out, *obj));
        if (const JValue* sec = obj->get("schematic")) {
            if (QStringView(out).mid(sec->begin, sec->end - sec->begin) != section) out = replaceValue(out, *sec, section);
        } else {
            out = setMember(out, *obj, QStringLiteral("schematic"), section);
        }
    }

    // 3. removed components, from the last to the first
    {
        JValue now;
        parseJsonc(out, now);
        const JValue* comps = componentsOf(now, scope);
        const int n = comps ? int(comps->items.size()) : 0;
        for (int i = n - 1; i >= 0; --i) {
            if (kept.contains(i)) continue;
            parseJsonc(out, now);
            out = removeItem(out, *componentsOf(now, scope), i);
        }
    }

    // 4. new components at the end
    QStringList added;
    for (int k = 0; k < after.components.size(); ++k)
        if (after.components[k].index < 0) added << componentJson(after.components[k], conn.pinNodes[k]);
    if (!added.isEmpty()) {
        JValue now;
        parseJsonc(out, now);
        if (!componentsOf(now, scope)) {
            if (scopeOf(now, scope)->get("components")) return out;  // not an array: leave it to the check
            out = setMember(out, *scopeOf(now, scope), QStringLiteral("components"), QStringLiteral("[]"));
            parseJsonc(out, now);
        }
        out = appendItems(out, *componentsOf(now, scope), added);
    }

    // 5. ports drawn with a new name extend the port list of the subcircuit
    if (!scope.isEmpty()) {
        QStringList fresh;
        for (const SLabel& l : after.labels)
            if (l.port && !after.ports.contains(l.name) && !fresh.contains(l.name)) fresh << jsonQuote(l.name);
        if (!fresh.isEmpty()) {
            JValue now;
            parseJsonc(out, now);
            const JValue* obj = scopeOf(now, scope);
            const JValue* ports = obj->get("ports");
            if (ports && ports->isArray()) out = appendItems(out, *ports, fresh);
            else if (!ports) out = setMember(out, *obj, QStringLiteral("ports"), QStringLiteral("[") + fresh.join(", ") + "]");
        }
    }
    return out;
}

QString setComponentMember(const QString& text, int index, const QString& key, const QString& json,
                           const QString& scope) {
    JValue root;
    if (!parseJsonc(text, root)) return text;
    const JValue* comps = componentsOf(root, scope);
    if (!comps || index < 0 || index >= int(comps->items.size()) || !comps->items[size_t(index)].isObject())
        return text;
    const JValue& obj = comps->items[size_t(index)];
    return json.isEmpty() ? removeMember(text, obj, key) : setMember(text, obj, key, json);
}

bool locateComponent(const QString& text, int pos, QString& scope, int& index) {
    JValue root;
    if (!parseJsonc(text, root) || !root.isObject()) return false;
    QStringList scopes{QString()};
    if (const JValue* subs = root.get("subcircuits"); subs && subs->isObject()) scopes += subs->keys;
    for (const QString& sc : scopes) {
        const JValue* comps = componentsOf(root, sc);
        if (!comps) continue;
        for (size_t k = 0; k < comps->items.size(); ++k) {
            if (pos >= comps->items[k].begin && pos <= comps->items[k].end) {
                scope = sc;
                index = int(k);
                return true;
            }
        }
    }
    return false;
}

QString addSubcircuit(const QString& text, const QString& name, const QStringList& ports) {
    JValue root;
    if (!parseJsonc(text, root) || !root.isObject()) return text;
    QStringList quoted;
    for (const QString& p : ports) quoted << jsonQuote(p);
    const auto definition = [&](const QString& indent) {
        const QString in = indent + QStringLiteral("  ");
        return QStringLiteral("{\n") + in + QStringLiteral("\"ports\": [") + quoted.join(", ") + QStringLiteral("],\n") +
               in + QStringLiteral("\"components\": []\n") + indent + QStringLiteral("}");
    };
    const JValue* subs = root.get("subcircuits");
    if (subs && subs->isObject()) {
        if (subs->get(name)) return text;
        return setMember(text, *subs, name, definition(memberIndent(text, *subs)));
    }
    if (subs) return text;  // not an object: leave it to the check
    const QString indent = memberIndent(text, root), in = indent + QStringLiteral("  ");
    return setMember(text, root, QStringLiteral("subcircuits"),
                     QStringLiteral("{\n") + in + jsonQuote(name) + QStringLiteral(": ") + definition(in) +
                         QStringLiteral("\n") + indent + QStringLiteral("}"));
}

Extraction extractSubcircuit(const QString& text, const Schematic& s, const ItemSet& sel, const QString& name) {
    Extraction out;
    if (name.trimmed().isEmpty() || name.contains(QRegularExpression(QStringLiteral("[\\s\"/]")))) {
        out.error = QStringLiteral("Der Name darf nicht leer sein und keine Leerzeichen, \" oder / enthalten");
        return out;
    }
    if (s.subcircuits.contains(name)) {
        out.error = QStringLiteral("Eine Teilschaltung „%1“ gibt es schon").arg(name);
        return out;
    }
    QVector<int> chosen;
    for (int k = 0; k < s.components.size(); ++k)
        if (sel.comps.contains(s.components[k].name)) chosen << k;
    if (chosen.isEmpty()) {
        out.error = QStringLiteral("Es ist kein Bauteil ausgewählt");
        return out;
    }
    for (int k : chosen)
        if (s.components[k].index < 0 || s.components[k].begin < 0) {
            out.error = QStringLiteral("Der Plan ist nicht auf dem Stand des Texts");
            return out;
        }
    const Connectivity c = connectSchematic(s);

    // nets: inside (a selected pin) and outside (an unselected pin or label)
    const int nets = int(c.netNames.size());
    QVector<bool> inside(nets, false), outside(nets, false);
    QVector<QPointF> where(nets, QPointF(1e9, 1e9));  // leftmost selected pin, for the order
    for (int k = 0; k < s.components.size(); ++k) {
        const bool picked = sel.comps.contains(s.components[k].name);
        for (int p = 0; p < c.pinNet[k].size(); ++p) {
            const int n = c.pinNet[k][p];
            (picked ? inside : outside)[n] = true;
            const QPoint at = c.pinPos[k][p];
            if (picked && (at.x() < where[n].x() || (at.x() == where[n].x() && at.y() < where[n].y()))) where[n] = at;
        }
    }
    for (int k = 0; k < s.labels.size(); ++k)
        if (!sel.labels.contains(k)) outside[c.labelNet[k]] = true;
    QVector<int> portNets;
    for (int n = 0; n < nets; ++n)
        if (inside[n] && outside[n] && !isGroundName(c.netNames[n])) portNets << n;
    std::sort(portNets.begin(), portNets.end(), [&](int a, int b) {
        return where[a].x() != where[b].x() ? where[a].x() < where[b].x() : where[a].y() < where[b].y();
    });
    for (int n : portNets) out.ports << c.netNames[n];

    // the drawing inside, moved to the origin
    Schematic inner;
    QRect box;
    for (int k : chosen) {
        const SComponent& comp = s.components[k];
        QRect r = comp.place.transform().mapRect(symbolOf(comp).body).toAlignedRect();
        for (const QPoint& p : c.pinPos[k]) r |= QRect(p, QSize(1, 1));
        box = box.isNull() ? r : box.united(r);
    }
    const QPoint shift(-box.left() + 2, -box.top() + 2);
    for (int k : chosen) {
        SComponent comp = s.components[k];
        comp.place.at += shift;
        inner.components << comp;
    }
    for (int k : sel.wires) {
        SWire w = s.wires.value(k);
        for (QPoint& p : w.pts) p += shift;
        inner.wires << w;
    }
    for (int k : sel.labels) {
        SLabel l = s.labels.value(k);
        l.at += shift;
        inner.labels << l;
    }
    for (int k : sel.grounds) {
        SGround g = s.grounds.value(k);
        g.at += shift;
        inner.grounds << g;
    }
    // where the rest touches the selection on a port net: there the net leaves.
    // Inside a port sits there, outside a label (pointing back into the gap).
    struct Contact {
        int net;
        QPoint at;
        QPoint out;  // direction away from the selection
    };
    QVector<Contact> contacts;
    QHash<QPoint, QPoint> selectedPins;  // position -> its outward direction
    for (int k : chosen)
        for (int p = 0; p < c.pinPos[k].size(); ++p)
            selectedPins.insert(c.pinPos[k][p], s.components[k].place.mapDir(symbolOf(s.components[k]).pins[p].dir));
    const auto sign = [](int v) { return v > 0 ? 1 : v < 0 ? -1 : 0; };
    // the outward direction at p if p lies on the selection, else (0, 0)
    const auto onSelection = [&](QPoint p) -> QPoint {
        if (selectedPins.contains(p)) return selectedPins.value(p);
        for (int k : sel.wires) {
            const QVector<QPoint>& w = s.wires.value(k).pts;
            for (int i = 0; i + 1 < w.size(); ++i) {
                const QPoint a = w[i], b = w[i + 1];
                const bool on = a.x() == b.x() ? p.x() == a.x() && p.y() >= std::min(a.y(), b.y()) && p.y() <= std::max(a.y(), b.y())
                                               : p.y() == a.y() && p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x());
                if (!on) continue;
                // at an end: onwards along the wire; in between: across it
                if (p == w.first()) return QPoint(sign(p.x() - w[1].x()), sign(p.y() - w[1].y()));
                if (p == w.last()) return QPoint(sign(p.x() - w[w.size() - 2].x()), sign(p.y() - w[w.size() - 2].y()));
                return a.x() == b.x() ? QPoint(-1, 0) : QPoint(0, -1);
            }
        }
        return QPoint();
    };
    QSet<QPoint> seen;
    const auto contact = [&](int net, QPoint p) {
        if (!portNets.contains(net) || seen.contains(p)) return;
        const QPoint out = onSelection(p);
        if (out.isNull()) return;
        seen.insert(p);
        contacts.push_back({net, p, out});
    };
    for (int k = 0; k < s.components.size(); ++k)
        if (!sel.comps.contains(s.components[k].name))
            for (int p = 0; p < c.pinPos[k].size(); ++p) contact(c.pinNet[k][p], c.pinPos[k][p]);
    for (int k = 0; k < s.wires.size(); ++k)
        if (!sel.wires.contains(k))
            for (const QPoint& v : s.wires[k].pts) contact(c.wireNet[k], v);
    for (int k = 0; k < s.labels.size(); ++k)
        if (!sel.labels.contains(k)) contact(c.labelNet[k], s.labels[k].at);
    for (int n : portNets) {
        bool placed = false;
        for (const Contact& ct : contacts)
            if (ct.net == n) {
                inner.labels.push_back({c.netNames[n], ct.at + shift, labelRot(ct.out), true});
                placed = true;
            }
        if (placed) continue;
        // no contact in the drawing (a name only): at a selected pin
        for (int k : chosen) {
            const int p = int(c.pinNet[k].indexOf(n));
            if (p < 0) continue;
            inner.labels.push_back({c.netNames[n], c.pinPos[k][p] + shift,
                                    labelRot(s.components[k].place.mapDir(symbolOf(s.components[k]).pins[p].dir)), true});
            break;
        }
    }

    // the definition: the components as written, with the nodes of the drawing
    QString result = text;
    {
        JValue root;
        parseJsonc(text, root);
        const JValue* subs = root.get("subcircuits");
        if (subs && !subs->isObject()) {
            out.error = QStringLiteral("\"subcircuits\" ist kein Objekt");
            return out;
        }
        const QString outer = subs ? memberIndent(text, *subs) : memberIndent(text, root) + QStringLiteral("  ");
        const QString in1 = outer + QStringLiteral("  "), in2 = in1 + QStringLiteral("  ");
        QStringList comps;
        for (int k : chosen) {
            QString obj = text.mid(s.components[k].begin, s.components[k].end - s.components[k].begin);
            if (c.pinNodes[k].size() == s.components[k].nodes.size() && c.pinNodes[k] != s.components[k].nodes) {
                // the drawing says otherwise than the text: the drawing counts
                JValue o;
                parseJsonc(obj, o);
                if (const JValue* nodes = o.get("nodes")) {
                    QStringList q;
                    for (const QString& node : c.pinNodes[k]) q << jsonQuote(node);
                    obj = replaceValue(obj, *nodes, QStringLiteral("[") + q.join(", ") + QStringLiteral("]"));
                }
            }
            comps << obj;
        }
        QStringList quoted;
        for (const QString& p : out.ports) quoted << jsonQuote(p);
        const QString def = QStringLiteral("{\n") + in1 + QStringLiteral("\"ports\": [") + quoted.join(", ") +
                            QStringLiteral("],\n") + in1 + QStringLiteral("\"components\": [\n") + in2 +
                            comps.join(QStringLiteral(",\n") + in2) + QStringLiteral("\n") + in1 + QStringLiteral("],\n") +
                            in1 + QStringLiteral("\"schematic\": ") + sectionJson(inner, in1) + QStringLiteral("\n") + outer +
                            QStringLiteral("}");
        if (subs) {
            result = setMember(text, *subs, name, def);
        } else {
            const QString top = memberIndent(text, root);
            result = setMember(text, root, QStringLiteral("subcircuits"),
                               QStringLiteral("{\n") + outer + jsonQuote(name) + QStringLiteral(": ") + def +
                                   QStringLiteral("\n") + top + QStringLiteral("}"));
        }
    }

    // the drawing outside: the selection gives way to an instance with labels
    Schematic after = s;
    for (int k = int(after.components.size()) - 1; k >= 0; --k)
        if (sel.comps.contains(after.components[k].name)) after.components.removeAt(k);
    const auto drop = [](auto& list, const QSet<int>& which) {
        QList<int> idx(which.begin(), which.end());
        std::sort(idx.begin(), idx.end(), std::greater<int>());
        for (int k : idx)
            if (k < list.size()) list.removeAt(k);
    };
    drop(after.wires, sel.wires);
    drop(after.labels, sel.labels);
    drop(after.grounds, sel.grounds);
    // the rest keeps its connections: a label at each contact
    for (const Contact& ct : contacts) {
        bool named = false;  // a label of that name is there already
        for (int l = 0; l < s.labels.size(); ++l)
            named = named || (!sel.labels.contains(l) && s.labels[l].at == ct.at && s.labels[l].name == c.netNames[ct.net]);
        if (!named)
            after.labels.push_back({c.netNames[ct.net], ct.at, labelRot(-ct.out), s.ports.contains(c.netNames[ct.net])});
    }
    SComponent x;
    x.type = QStringLiteral("X");
    x.kind = 'x';
    x.subcircuit = name;
    x.ports = out.ports;
    x.name = after.freeName(QStringLiteral("X"));
    x.summary = name;
    x.nodes = QStringList(QVector<QString>(out.ports.size()));
    x.place.at = QPoint(box.center().x(), box.center().y());
    x.placed = true;
    const Symbol sym = symbolOf(x);
    for (int p = 0; p < sym.pins.size(); ++p)
        after.labels.push_back({out.ports[p], x.place.map(sym.pins[p].pos), labelRot(sym.pins[p].dir), s.ports.contains(out.ports[p])});
    after.components << x;
    out.instance = x.name;
    out.text = writeSchematic(result, after);
    return out;
}

QVector<Instance> instancesOf(const QString& text, const QString& subcircuit) {
    QVector<Instance> out;
    JValue root;
    if (!parseJsonc(text, root)) return out;
    const JValue* comps = componentsOf(root, QString());
    if (!comps) return out;
    for (size_t k = 0; k < comps->items.size(); ++k) {
        const JValue& o = comps->items[k];
        if (!o.isObject() || kindOf(member(text, o, "type")) != 'x' || member(text, o, "subcircuit") != subcircuit)
            continue;
        Instance in;
        in.name = member(text, o, "name");
        if (in.name.isEmpty()) in.name = defaultName('x', int(k));
        if (const JValue* n = o.get("nodes"); n && n->isArray())
            for (const JValue& v : n->items) in.nodes << v.text(text);
        out << in;
    }
    return out;
}

// --- palette ----------------------------------------------------------------------

const QVector<PartTemplate>& partTemplates() {
    static const QVector<PartTemplate> parts = {
        {"r", QString::fromUtf8("Widerstand"), "R", 'r', "R", "", 2, R"("value": "1k")"},
        {"c", QString::fromUtf8("Kondensator"), "C", 'c', "C", "", 2, R"("value": "1u")"},
        {"l", QString::fromUtf8("Spule"), "L", 'l', "L", "", 2, R"("value": "1m")"},
        {"v", QString::fromUtf8("Spannungsquelle"), "V", 'v', "V", "", 2, R"("dc": 1)"},
        {"vsin", QString::fromUtf8("Sinusquelle"), "V", 'v', "V", "sin", 2,
         R"("dc": 0, "ac": 1, "wave": { "type": "sin", "amplitude": 1, "freq": "1k" })"},
        {"vpulse", QString::fromUtf8("Pulsquelle"), "V", 'v', "V", "pulse", 2,
         R"("wave": { "type": "pulse", "v1": 0, "v2": 1, "rise": "1u", "fall": "1u", "width": "0.5m", "period": "1m" })"},
        {"i", QString::fromUtf8("Stromquelle"), "I", 'i', "I", "", 2, R"("dc": "1m")"},
        {"d", QString::fromUtf8("Diode"), "D", 'd', "D", "", 2, ""},
        {"ideal_diode", QString::fromUtf8("Ideale Diode"), "ideal_diode", 'y', "D", "", 2, R"("vf": 0.7)"},
        {"npn", QString::fromUtf8("NPN-Transistor"), "npn", 'q', "Q", "npn", 3, ""},
        {"pnp", QString::fromUtf8("PNP-Transistor"), "pnp", 'q', "Q", "pnp", 3, ""},
        {"nmos", QString::fromUtf8("N-MOSFET"), "nmos", 'm', "M", "nmos", 3, ""},
        {"pmos", QString::fromUtf8("P-MOSFET"), "pmos", 'm', "M", "pmos", 3, ""},
        {"njf", QString::fromUtf8("N-JFET"), "njf", 'j', "J", "njf", 3, ""},
        {"pjf", QString::fromUtf8("P-JFET"), "pjf", 'j', "J", "pjf", 3, ""},
        {"e", QString::fromUtf8("Spannungsgesteuerte Spannungsquelle"), "E", 'e', "E", "", 4, R"("gain": 1)"},
        {"g", QString::fromUtf8("Spannungsgesteuerte Stromquelle"), "G", 'g', "G", "", 4, R"("gm": "1m")"},
        {"f", QString::fromUtf8("Stromgesteuerte Stromquelle"), "F", 'f', "F", "", 2, R"("control": "V1", "gain": 1)"},
        {"h", QString::fromUtf8("Stromgesteuerte Spannungsquelle"), "H", 'h', "H", "", 2, R"("control": "V1", "r": "1k")"},
        {"bv", QString::fromUtf8("Verhaltensquelle, Spannung"), "B", 'b', "B", "v", 2, R"("v": "0")"},
        {"bi", QString::fromUtf8("Verhaltensquelle, Strom"), "B", 'b', "B", "i", 2, R"("i": "0")"},
        {"s", QString::fromUtf8("Schalter, spannungsgesteuert"), "S", 's', "S", "", 4, R"("vt": 0.5)"},
        {"t", QString::fromUtf8("Leitung"), "T", 't', "T", "", 4, R"("z0": 50, "td": "1n")"},
        {"k", QString::fromUtf8("Kopplung zweier Spulen"), "K", 'k', "K", "", 0, R"("inductors": ["L1", "L2"], "k": 0.99)"},
    };
    return parts;
}

SComponent makeComponent(const PartTemplate& t, const Schematic& s) {
    SComponent c;
    c.type = t.type;
    c.kind = t.kind;
    c.variant = t.variant;
    c.name = s.freeName(t.prefix);
    c.extra = t.extra;
    c.nodes = QStringList(QVector<QString>(t.pins));
    // the value as shown at the symbol, from the defaults
    static const QRegularExpression value(QStringLiteral(R"re("(?:value|dc|gain|gm|vf)": "?([^",]+)"?)re"));
    const auto m = value.match(t.extra);
    if (t.kind == 'k') c.ports = QStringList{"L1", "L2"};
    if (m.hasMatch()) c.summary = m.captured(1);
    return c;
}

SComponent makeSubcircuit(const QString& name, const Schematic& s) {
    SComponent c;
    c.type = QStringLiteral("X");
    c.kind = 'x';
    c.subcircuit = name;
    c.ports = s.subcircuits.value(name);
    c.name = s.freeName(QStringLiteral("X"));
    c.summary = name;
    c.nodes = QStringList(QVector<QString>(c.ports.size()));
    return c;
}
