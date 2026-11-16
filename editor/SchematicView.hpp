#pragma once

#include <QHash>
#include <QSet>
#include <QWidget>

#include "Schematic.hpp"

// The drawing area of the schematic. It shows a Schematic read from the
// text and turns every change (place, move, wire, delete, ...) into a new
// Schematic that it hands out with edited(); the main window writes it
// into the text, reads it back and passes it in again with setSchematic().
//
// Mouse: click selects (⇧ adds), drag moves or selects with a frame,
// double click opens the properties, ⌥-click plots the voltage of a net
// or the current of a component. Scroll pans (trackpad) or zooms (mouse
// wheel, ⌘ + scroll, pinch).
// Keys: R rotate, X mirror, Del delete, ⌘D duplicate, arrows move,
// W wire, G ground, N label, P port (in a subcircuit), F fit, Esc cancel,
// ⌘↑ back to the main circuit.
class SchematicView : public QWidget {
    Q_OBJECT
public:
    explicit SchematicView(QWidget* parent = nullptr);

    void setSchematic(const Schematic& s);
    const Schematic& schematic() const { return model_; }
    const Connectivity& connectivity() const { return conn_; }
    // The text cannot be read: the drawing shows the last valid state and
    // cannot be changed.
    void setStale(bool stale);
    // The next paint fits the drawing into the window (a new file).
    void resetView();

    // Selects components (by name) without emitting selectionChanged.
    void selectComponents(const QStringList& names);
    QStringList selectedComponents() const;
    const ItemSet& selection() const { return sel_; }

    void startPlacing(const SComponent& c);
    void startGround();
    void startLabel();
    void startPort();  // only in a subcircuit
    void startWire();
    void fitAll();
    QPointF gridToWidget(QPointF grid) const { return toScreen(grid); }
    // Node voltages and currents of the operating point, shown at the labels
    // and in the tooltips ("v(out)" -> 2.5).
    void setOperatingPoint(const QHash<QString, double>& values);

signals:
    void edited(const Schematic& after);
    void selectionChanged(const QStringList& components);
    void componentActivated(const QString& name);
    void probe(const QString& quantity);
    void partChooserRequested();
    void openSubcircuit(const QString& name);  // double click on an instance
    void leaveRequested();                     // ⌘↑: back to the main circuit
    void extractRequested();                   // the selection into a new subcircuit

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void changeEvent(QEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

public:
    using Selection = ItemSet;  // components by name, the others by index
    enum class ItemType { None, Component, Wire, Label, Ground };
    struct Hit {
        ItemType type = ItemType::None;
        int index = -1;
    };

private:
    enum class Mode { Select, Place, Wire };
    enum class Ghost { Part, Ground, Label };
    enum class Drag { None, Pending, Move, Rubber, Pan };

    // view transform: grid -> widget pixels
    QPointF toScreen(QPointF g) const { return g * scale_ + pan_; }
    QPointF toGrid(QPointF px) const { return (px - pan_) / scale_; }
    QPoint snap(QPointF px) const;
    QTransform viewTransform() const;
    void zoomAt(QPointF px, double factor);

    Hit hitAt(QPointF px) const;
    QString netAt(QPointF px) const;
    void setSelection(const Selection& s, bool notify = true);
    bool selected(ItemType type, int index) const;
    Selection selectionIn(const QRectF& gridRect) const;
    QSet<QPoint> connectionPoints() const;

    bool canEdit();
    void commit(Schematic after, const Selection& selection);
    void deleteSelection();
    void duplicateSelection();
    void transformSelection(int how);  // 0 rotate, 1 mirror
    void moveSelection(QPoint delta);
    Schematic moved(QPoint delta) const;
    QVector<QPoint> route(QPoint from, QPoint to) const;
    void finishWire();
    void cancel();
    void renameLabel(int index);
    void contextMenu(QMouseEvent* e);

    void paintGrid(QPainter& p);
    void paintModel(QPainter& p, const Schematic& s, const Connectivity& c, bool ghost);
    void paintComponent(QPainter& p, const SComponent& c, const QColor& ink, bool conflictPins,
                        const QVector<int>& conflicts);
    void paintLabel(QPainter& p, const SLabel& l, const QColor& ink, const QString& extra);
    void paintGround(QPainter& p, const SGround& g, const QColor& ink);
    void paintText(QPainter& p, QPointF at, const QString& text, int align, double size, const QColor& color);
    double penWidth() const;
    double fontPixels(double gridSize) const;

    Schematic model_;
    Connectivity conn_;
    Schematic display_;  // during a move: the moved state
    Connectivity displayConn_;
    bool stale_ = false;
    QHash<QString, double> op_;

    Selection sel_;
    Hit hover_;
    QString hoverNet_;

    Mode mode_ = Mode::Select;
    Ghost ghostKind_ = Ghost::Part;
    SComponent ghost_;
    SLabel ghostLabel_;
    SGround ghostGround_;
    QVector<QPoint> wirePts_;  // the wire being drawn
    bool horizontalFirst_ = true;
    QPoint mouseGrid_;
    bool mouseInside_ = false;

    Drag drag_ = Drag::None;
    QPointF pressPx_;
    QPoint pressGrid_;
    QPoint moveDelta_;
    QPointF lastPanPx_;
    QRectF rubber_;  // widget pixels
    bool spaceHeld_ = false;

    double scale_ = 14.0;  // pixels per grid unit
    QPointF pan_{60, 60};
    bool fitted_ = false;
};
