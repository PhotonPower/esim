#include "HistogramWidget.hpp"

#include <QContextMenuEvent>
#include <QFileDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <cmath>

#include "Format.hpp"
#include "Theme.hpp"

namespace {

constexpr int kLeft = 56, kRight = 20, kGapBetween = 14;

// tick spacing of 1, 2 or 5 times a power of ten, about `count` ticks
double niceStep(double range, int count) {
    if (!(range > 0)) return 1.0;
    const double raw = range / count, p = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / p;
    return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * p;
}

}  // namespace

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(160);
}

void HistogramWidget::setData(const QVector<Histogram>& histograms) {
    data_ = histograms;
    hoverPanel_ = hoverBin_ = -1;
    update();
}

QRect HistogramWidget::area(int k) const {
    const QFontMetrics fm(font());
    const int n = std::max(1, int(data_.size()));
    const int slot = (height() - 8) / n;
    const int top = 4 + k * slot + fm.height() + 8;              // the title above
    const int bottom = 4 + (k + 1) * slot - 2 * fm.height() - 6;  // tick labels and gap below
    return QRect(QPoint(kLeft, top), QPoint(width() - kRight, std::max(top + 20, bottom - (k + 1 < n ? kGapBetween : 0))));
}

QRectF HistogramWidget::bar(int k, int bin) const {
    const Histogram& h = data_[k];
    const QRect a = area(k);
    const double lo = h.edges.first(), hi = h.edges.last();
    int top = 0;
    for (int c : h.counts) top = std::max(top, c);
    const double yMax = top * 1.1;
    const auto px = [&](double x) { return a.left() + (x - lo) / (hi - lo) * a.width(); };
    const double x0 = px(h.edges[bin]) + 1.0, x1 = px(h.edges[bin + 1]) - 1.0;  // 2 px between bars
    const double y1 = a.bottom() + 1.0, y0 = y1 - h.counts[bin] / yMax * a.height();
    return QRectF(QPointF(x0, y0), QPointF(std::max(x0 + 1.0, x1), y1));
}

void HistogramWidget::paintPanel(QPainter& p, int k) {
    const Histogram& h = data_[k];
    const QRect a = area(k);
    const QFontMetrics fm(font());
    if (h.count == 0 || h.edges.size() < 2) {
        p.setPen(Theme::textSecondary());
        p.drawText(a, Qt::AlignCenter, tr("%1: keine Werte").arg(h.name));
        return;
    }
    const double lo = h.edges.first(), hi = h.edges.last();
    const auto px = [&](double x) { return a.left() + (x - lo) / (hi - lo) * a.width(); };
    int top = 0;
    for (int c : h.counts) top = std::max(top, c);
    const double yMax = top * 1.1;

    // title: the quantity and its statistics
    p.setPen(Theme::textPrimary());
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    const QRect titleRect(a.left(), a.top() - fm.height() - 6, a.width(), fm.height());
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, h.name);
    const int nameWidth = QFontMetrics(bold).horizontalAdvance(h.name);
    p.setFont(font());
    p.setPen(Theme::textSecondary());
    const QString stats = QString::fromUtf8("n = %1   μ = %2   σ = %3   σ/μ = %4 %   min %5   max %6")
                              .arg(h.count)
                              .arg(formatSI(h.mean, QString(), 4), formatSI(h.std, QString(), 3))
                              .arg(h.mean != 0 ? QString::number(100.0 * h.std / std::abs(h.mean), 'f', 2) : QStringLiteral("–"))
                              .arg(formatSI(h.min, QString(), 4), formatSI(h.max, QString(), 4));
    p.drawText(titleRect.adjusted(nameWidth + 14, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(stats, Qt::ElideRight, titleRect.width() - nameWidth - 14));

    // grid and count axis
    const double yStep = std::max(1.0, niceStep(yMax, 4));
    for (double y = 0; y <= yMax; y += yStep) {
        const double py = a.bottom() - y / yMax * a.height();
        p.setPen(QPen(Theme::grid(), 1));
        p.drawLine(QPointF(a.left(), py), QPointF(a.right(), py));
        p.setPen(Theme::textSecondary());
        p.drawText(QRect(0, int(py) - fm.height() / 2, kLeft - 8, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(int(std::lround(y))));
    }
    // value axis
    const double xStep = niceStep(hi - lo, std::max(2, a.width() / 90));
    p.setPen(Theme::textSecondary());
    for (double x = std::ceil(lo / xStep) * xStep; x <= hi + 1e-9 * xStep; x += xStep) {
        const double qx = px(x);
        p.drawLine(QPointF(qx, a.bottom() + 1), QPointF(qx, a.bottom() + 5));
        const QString t = formatSI(x, QString(), 4);
        const int w = fm.horizontalAdvance(t);
        const int left = std::clamp(int(qx) - w / 2, 0, width() - w);
        p.drawText(QRect(left, a.bottom() + 6, w, fm.height()), Qt::AlignCenter, t);
    }
    // ±σ band, bars, mean
    QColor band = Theme::series(h.slot);
    band.setAlpha(Theme::dark() ? 34 : 22);
    const double s0 = std::max(double(a.left()), px(h.mean - h.std)), s1 = std::min(double(a.right()), px(h.mean + h.std));
    if (s1 > s0) p.fillRect(QRectF(QPointF(s0, a.top()), QPointF(s1, a.bottom())), band);
    p.setPen(Qt::NoPen);
    for (int b = 0; b < h.counts.size(); ++b) {
        if (h.counts[b] == 0) continue;
        const QRectF r = bar(k, b);
        QColor c = Theme::series(h.slot);
        if (k == hoverPanel_ && b == hoverBin_) c = Theme::dark() ? c.lighter(125) : c.darker(115);
        p.setBrush(c);
        // the end away from the baseline rounded
        const double rad = std::min({3.0, r.width() / 2, r.height()});
        QPainterPath path;
        path.moveTo(r.bottomLeft());
        path.lineTo(r.left(), r.top() + rad);
        path.quadTo(r.topLeft(), QPointF(r.left() + rad, r.top()));
        path.lineTo(r.right() - rad, r.top());
        path.quadTo(r.topRight(), QPointF(r.right(), r.top() + rad));
        path.lineTo(r.bottomRight());
        path.closeSubpath();
        p.drawPath(path);
    }
    p.setPen(QPen(Theme::axis(), 1));
    p.drawLine(a.bottomLeft(), a.bottomRight());
    const double m = px(h.mean);
    p.setPen(QPen(Theme::textPrimary(), 1.5));
    p.drawLine(QPointF(m, a.top()), QPointF(m, a.bottom()));
    // the label of the mean on a patch of background, above the bars
    const QString mu = QString::fromUtf8("μ ± σ");
    const QRectF tag(m + 4, a.top() + 2, fm.horizontalAdvance(mu) + 8, fm.height() + 2);
    QColor patch = Theme::surface();
    patch.setAlpha(225);
    p.setPen(Qt::NoPen);
    p.setBrush(patch);
    p.drawRoundedRect(tag, 3, 3);
    p.setPen(Theme::textSecondary());
    p.drawText(tag, Qt::AlignCenter, mu);
    Q_UNUSED(yMax);
}

void HistogramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::surface());
    if (data_.isEmpty()) {
        p.setPen(Theme::textSecondary());
        p.drawText(rect(), Qt::AlignCenter, tr("Keine Messgröße gewählt"));
        return;
    }
    for (int k = 0; k < data_.size(); ++k) paintPanel(p, k);
}

void HistogramWidget::mouseMoveEvent(QMouseEvent* e) {
    int panel = -1, bin = -1;
    for (int k = 0; k < data_.size() && panel < 0; ++k) {
        const QRect a = area(k);
        if (!a.adjusted(0, -4, 0, 4).contains(e->pos()) || data_[k].edges.size() < 2) continue;
        for (int b = 0; b < data_[k].counts.size(); ++b) {
            const QRectF r = bar(k, b);
            if (e->position().x() >= r.left() - 1 && e->position().x() <= r.right() + 1) {
                panel = k;
                bin = b;
                break;
            }
        }
    }
    if (panel == hoverPanel_ && bin == hoverBin_) return;
    hoverPanel_ = panel;
    hoverBin_ = bin;
    update();
    if (panel < 0) {
        QToolTip::hideText();
        return;
    }
    const Histogram& h = data_[panel];
    QToolTip::showText(e->globalPosition().toPoint(),
                       tr("%1 … %2\n%3 Läufe (%4 %)")
                           .arg(formatSI(h.edges[bin], QString(), 4), formatSI(h.edges[bin + 1], QString(), 4))
                           .arg(h.counts[bin])
                           .arg(QString::number(100.0 * h.counts[bin] / std::max(1, h.count), 'f', 1)),
                       this);
}

void HistogramWidget::leaveEvent(QEvent*) {
    hoverPanel_ = hoverBin_ = -1;
    update();
}

bool HistogramWidget::saveImage(const QString& path) { return grab().save(path); }

void HistogramWidget::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    menu.addAction(tr("Als Bild speichern …"), this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, tr("Histogramm als Bild"), "histogramm.png",
                                                          tr("Bilder (*.png *.jpg)"));
        if (!path.isEmpty()) saveImage(path);
    });
    menu.exec(e->globalPos());
}
