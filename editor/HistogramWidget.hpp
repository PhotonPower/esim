#pragma once

#include <QWidget>

#include "ResultData.hpp"

// Histograms of measured quantities of a Monte Carlo analysis, one panel
// per quantity (each with its own axis: the quantities differ in unit and
// scale). The mean and ±σ are marked; hovering a bar shows its range and
// the number of runs in it.
class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistogramWidget(QWidget* parent = nullptr);
    void setData(const QVector<Histogram>& histograms);
    void clear() { setData({}); }
    bool saveImage(const QString& path);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    QRect area(int panel) const;  // plot area of a panel
    QRectF bar(int panel, int bin) const;
    void paintPanel(class QPainter& p, int k);

    QVector<Histogram> data_;
    int hoverPanel_ = -1, hoverBin_ = -1;
};
