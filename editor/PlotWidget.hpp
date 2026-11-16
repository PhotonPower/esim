#pragma once

#include <QPixmap>
#include <QWidget>

#include "ResultData.hpp"

// Curves in panels above each other on a common x axis.
//
//   scroll / pinch      zoom in x around the pointer (with ⌘/Ctrl: y of the panel)
//   horizontal scroll   move in x
//   drag                zoom to the rectangle (a flat one: x only)
//   double click        show everything
//   hover               crosshair with the values of the curves of the panel
class PlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget* parent = nullptr);

    void setData(const PlotData& data);  // keeps the x range if the x quantity is the same
    void clear();
    void fitAll();
    bool saveImage(const QString& path);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;
    bool event(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    struct Range {
        double lo = 0.0, hi = 1.0;
    };
    struct PaneView {
        Range y;
        bool autoY = true;  // follows the visible curves
        QRect area;         // plot area in widget coordinates
    };

    void layoutPanes();
    void autoscaleY(int pane);
    Range dataX() const;
    double toX(double x) const;     // data -> axis coordinate (log10 for a log axis)
    double fromX(double u) const;
    double pixelX(double u) const;  // axis coordinate -> pixel
    double axisX(double px) const;  // pixel -> axis coordinate
    double pixelY(int pane, double y) const;
    double valueY(int pane, double py) const;
    int paneAt(const QPoint& p) const;
    void zoomX(double factor, double atPixel);
    void zoomY(int pane, double factor, double atPixel);

    void drawPane(QPainter& p, int k);
    void drawCurve(QPainter& p, const Curve& c, int pane, bool sorted);
    void drawXAxis(QPainter& p);
    void drawLegend(QPainter& p);
    void drawCrosshair(QPainter& p);

    void renderStatic(QPainter& p);  // everything except crosshair and drag rectangle

    PlotData data_;
    QVector<QVector<char>> ascending_;  // per panel and curve: x ascending (binary search possible)
    // Curves and axes as last drawn, redrawn only when what they show changes.
    QPixmap cache_;
    QVector<double> cacheKey_;
    int generation_ = 0;  // counts setData()
    QVector<PaneView> panes_;
    Range x_;  // visible x range in axis coordinates
    QRect legendArea_;
    bool hover_ = false;
    QPoint mouse_;
    bool dragging_ = false;
    QPoint dragStart_;
};
