#include "PlotWidget.hpp"

#include <QContextMenuEvent>
#include <QFileDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

#include "Format.hpp"
#include "Keys.hpp"
#include "Theme.hpp"

namespace {

constexpr int kLeft = 72;     // room for the tick labels of y
constexpr int kRight = 16;
constexpr int kBottom = 46;   // tick labels of x and the axis title below them
constexpr int kTitle = 18;    // title line above each panel
constexpr int kGap = 8;       // between panels
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double niceStep(double span, int maxTicks) {
    const double raw = span / std::max(maxTicks, 1);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double n = raw / mag;
    return (n < 1.5 ? 1.0 : n < 3.0 ? 2.0 : n < 7.0 ? 5.0 : 10.0) * mag;
}

Qt::PenStyle penStyle(int dash) {
    switch (dash) {
        case 1: return Qt::DashLine;
        case 2: return Qt::DotLine;
        case 3: return Qt::DashDotLine;
        default: return Qt::SolidLine;
    }
}

bool ascending(const QVector<double>& x) {
    for (int k = 1; k < x.size(); ++k)
        if (x[k] < x[k - 1]) return false;
    return true;
}

// y at x on the curve (linear between the points); NaN outside of it.
double interpolate(const Curve& c, double x) {
    if (c.x.isEmpty() || x < c.x.front() || x > c.x.back()) return kNaN;
    const auto it = std::lower_bound(c.x.begin(), c.x.end(), x);
    const int k = static_cast<int>(it - c.x.begin());
    if (k == 0) return c.y[0];
    const double x0 = c.x[k - 1], x1 = c.x[k];
    if (x1 == x0) return c.y[k];
    return c.y[k - 1] + (c.y[k] - c.y[k - 1]) * (x - x0) / (x1 - x0);
}

QString label(const Curve& c) { return c.group.isEmpty() ? c.name : c.name + ", " + c.group; }

}  // namespace

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(240, 160);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::ClickFocus);
}

void PlotWidget::setData(const PlotData& data) {
    const bool sameX = data.xLabel == data_.xLabel && data.logX == data_.logX && !data_.panels.isEmpty();
    const Range oldX = x_;
    data_ = data;
    // curves with descending x (a DC sweep downwards) are turned around
    for (Panel& panel : data_.panels)
        for (Curve& c : panel.curves)
            if (c.x.size() > 1 && c.x.front() > c.x.back()) {
                std::reverse(c.x.begin(), c.x.end());
                std::reverse(c.y.begin(), c.y.end());
            }
    ascending_.clear();
    for (const Panel& panel : data_.panels) {
        QVector<char> flags;
        for (const Curve& c : panel.curves) flags.push_back(ascending(c.x));
        ascending_.push_back(flags);
    }
    ++generation_;
    panes_ = QVector<PaneView>(data_.panels.size());
    const Range all = dataX();
    x_ = sameX && oldX.lo >= all.lo - 1e-12 * std::abs(all.lo) && oldX.hi <= all.hi + 1e-12 * std::abs(all.hi)
             ? oldX : all;
    layoutPanes();
    for (int k = 0; k < panes_.size(); ++k) autoscaleY(k);
    update();
}

void PlotWidget::clear() {
    data_ = {};
    panes_.clear();
    ascending_.clear();
    ++generation_;
    update();
}

void PlotWidget::fitAll() {
    x_ = dataX();
    for (int k = 0; k < panes_.size(); ++k) {
        panes_[k].autoY = true;
        autoscaleY(k);
    }
    update();
}

bool PlotWidget::saveImage(const QString& path) {
    QPixmap image(size() * 2);
    image.setDevicePixelRatio(2.0);
    const bool hover = hover_;
    hover_ = false;
    render(&image);
    hover_ = hover;
    return image.save(path);
}

// --- coordinates ------------------------------------------------------------

double PlotWidget::toX(double x) const {
    if (!data_.logX) return x;
    return x > 0.0 ? std::log10(x) : kNaN;
}

double PlotWidget::fromX(double u) const { return data_.logX ? std::pow(10.0, u) : u; }

PlotWidget::Range PlotWidget::dataX() const {
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const Panel& panel : data_.panels)
        for (const Curve& c : panel.curves)
            for (double x : c.x) {
                const double u = toX(x);
                if (!std::isfinite(u)) continue;
                lo = std::min(lo, u);
                hi = std::max(hi, u);
            }
    if (!(lo <= hi)) return {0.0, 1.0};
    if (hi == lo) return {lo - 0.5, hi + 0.5};
    return {lo, hi};
}

double PlotWidget::pixelX(double u) const {
    const QRect& a = panes_.isEmpty() ? rect() : panes_[0].area;
    return a.left() + (u - x_.lo) / (x_.hi - x_.lo) * a.width();
}

double PlotWidget::axisX(double px) const {
    const QRect& a = panes_.isEmpty() ? rect() : panes_[0].area;
    return x_.lo + (px - a.left()) / std::max(a.width(), 1) * (x_.hi - x_.lo);
}

double PlotWidget::pixelY(int pane, double y) const {
    const PaneView& v = panes_[pane];
    return v.area.bottom() - (y - v.y.lo) / (v.y.hi - v.y.lo) * v.area.height();
}

double PlotWidget::valueY(int pane, double py) const {
    const PaneView& v = panes_[pane];
    return v.y.lo + (v.area.bottom() - py) / std::max(v.area.height(), 1) * (v.y.hi - v.y.lo);
}

int PlotWidget::paneAt(const QPoint& p) const {
    for (int k = 0; k < panes_.size(); ++k)
        if (panes_[k].area.adjusted(-kLeft, -kTitle, kRight, 0).contains(p)) return k;
    return -1;
}

// --- layout and scaling -----------------------------------------------------

void PlotWidget::layoutPanes() {
    // legend: entries in rows over the full width
    const QFontMetrics fm(font());
    int x = kLeft, rows = data_.panels.isEmpty() ? 0 : 1;
    QStringList seen;
    for (const Panel& panel : data_.panels)
        for (const Curve& c : panel.curves) {
            if (seen.contains(c.name)) continue;
            seen.push_back(c.name);
            const int w = 28 + fm.horizontalAdvance(c.name) + 16;
            if (x + w > width() - kRight && x > kLeft) {
                ++rows;
                x = kLeft;
            }
            x += w;
        }
    const bool sweep = !data_.panels.isEmpty() && !data_.panels[0].curves.isEmpty() &&
                       data_.panels[0].curves[0].runs > 1;
    if (sweep) ++rows;
    const int legendHeight = rows * (fm.height() + 4) + (rows ? 6 : 0);
    legendArea_ = QRect(0, 0, width(), legendHeight);

    const int n = panes_.size();
    if (n == 0) return;
    const int top = legendHeight;
    const int available = height() - top - kBottom - n * kTitle - (n - 1) * kGap;
    const int h = std::max(available / n, 20);
    int y = top;
    for (int k = 0; k < n; ++k) {
        y += kTitle;
        panes_[k].area = QRect(kLeft, y, std::max(width() - kLeft - kRight, 20), h);
        y += h + kGap;
    }
}

void PlotWidget::autoscaleY(int pane) {
    PaneView& v = panes_[pane];
    if (!v.autoY) return;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const Curve& c : data_.panels[pane].curves) {
        for (int k = 0; k < c.x.size(); ++k) {
            const double u = toX(c.x[k]);
            if (!(u >= x_.lo && u <= x_.hi) || !std::isfinite(c.y[k])) continue;
            lo = std::min(lo, c.y[k]);
            hi = std::max(hi, c.y[k]);
        }
    }
    if (!(lo <= hi)) {
        v.y = {0.0, 1.0};
        return;
    }
    if (hi - lo <= 1e-12 * std::max(std::abs(hi), std::abs(lo))) {
        const double d = lo == 0.0 ? 1.0 : 0.05 * std::abs(lo);
        lo -= d;
        hi += d;
    }
    const double pad = 0.05 * (hi - lo);
    v.y = {lo - pad, hi + pad};
}

void PlotWidget::zoomX(double factor, double atPixel) {
    const double u = axisX(atPixel);
    Range r{u - (u - x_.lo) * factor, u + (x_.hi - u) * factor};
    if (r.hi - r.lo < 1e-12 * std::max(std::abs(r.lo), std::abs(r.hi)) + 1e-300) return;
    x_ = r;
    for (int k = 0; k < panes_.size(); ++k) autoscaleY(k);
    update();
}

void PlotWidget::zoomY(int pane, double factor, double atPixel) {
    PaneView& v = panes_[pane];
    const double y = valueY(pane, atPixel);
    v.y = {y - (y - v.y.lo) * factor, y + (v.y.hi - y) * factor};
    v.autoY = false;
    update();
}

// --- painting ---------------------------------------------------------------

void PlotWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Theme::surface());
    if (data_.panels.isEmpty()) {
        p.setPen(Theme::textSecondary());
        p.drawText(rect(), Qt::AlignCenter, tr("Keine Kurven: Simulation starten (%1) und eine Analyse wählen").arg(Keys::cmd("R")));
        return;
    }
    if (panes_.isEmpty() || panes_[0].area.width() != width() - kLeft - kRight) layoutPanes();

    QVector<double> key{double(generation_), x_.lo, x_.hi, double(width()), double(height()),
                        devicePixelRatioF(), double(Theme::dark())};
    for (const PaneView& v : panes_) key << v.y.lo << v.y.hi;
    if (key != cacheKey_ || cache_.isNull()) {
        cache_ = QPixmap(size() * devicePixelRatioF());
        cache_.setDevicePixelRatio(devicePixelRatioF());
        cache_.fill(Theme::surface());
        QPainter cp(&cache_);
        cp.setFont(font());
        renderStatic(cp);
        cacheKey_ = key;
    }
    p.drawPixmap(0, 0, cache_);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (dragging_) {
        const int k = paneAt(dragStart_);
        if (k >= 0) {
            QRect r = QRect(dragStart_, mouse_).normalized().intersected(panes_[k].area);
            if (std::abs(mouse_.y() - dragStart_.y()) < 6)
                r = QRect(r.left(), panes_[k].area.top(), r.width(), panes_[k].area.height());
            QColor fill = Theme::series(0);
            fill.setAlphaF(0.12f);
            p.fillRect(r, fill);
            p.setPen(QPen(Theme::series(0), 1));
            p.drawRect(r);
        }
    } else if (hover_) {
        drawCrosshair(p);
    }
}

void PlotWidget::renderStatic(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int k = 0; k < panes_.size(); ++k) drawPane(p, k);
    drawXAxis(p);
    drawLegend(p);
}

void PlotWidget::drawPane(QPainter& p, int k) {
    const PaneView& v = panes_[k];
    const Panel& panel = data_.panels[k];
    const QRect& a = v.area;
    const QFontMetrics fm(font());

    // title
    p.setPen(Theme::textSecondary());
    const QString title = panel.unit.isEmpty() ? panel.title : panel.title + " [" + panel.unit + "]";
    p.drawText(QRect(a.left(), a.top() - kTitle, a.width(), kTitle), Qt::AlignLeft | Qt::AlignVCenter, title);

    // y grid and labels
    const double step = niceStep(v.y.hi - v.y.lo, std::max(2, a.height() / 40));
    const bool plain = panel.unit == "dB" || panel.unit == QString::fromUtf8("°");
    const int exponent = plain ? 0 : siExponent(std::max(std::abs(v.y.lo), std::abs(v.y.hi)));
    const int decimals = std::max(0, static_cast<int>(-std::floor(std::log10(step) + 1e-9)) + exponent);
    for (double y = std::ceil(v.y.lo / step) * step; y <= v.y.hi; y += step) {
        const double py = pixelY(k, y);
        p.setPen(QPen(Theme::grid(), 1));
        p.drawLine(QPointF(a.left(), py), QPointF(a.right(), py));
        p.setPen(Theme::textSecondary());
        const QString text = formatWithExponent(y, exponent, decimals, panel.unit);
        p.drawText(QRectF(0, py - fm.height() / 2.0, kLeft - 8, fm.height()), Qt::AlignRight | Qt::AlignVCenter, text);
    }

    // x grid
    p.setPen(QPen(Theme::grid(), 1));
    if (data_.logX) {
        const bool minor = x_.hi - x_.lo < 6.0;
        for (double d = std::floor(x_.lo); d <= x_.hi; d += 1.0)
            for (int m = 1; m <= (minor ? 9 : 1); ++m) {
                const double u = d + std::log10(m);
                if (u < x_.lo || u > x_.hi) continue;
                const double px = pixelX(u);
                p.drawLine(QPointF(px, a.top()), QPointF(px, a.bottom()));
            }
    } else {
        const double xs = niceStep(x_.hi - x_.lo, std::max(2, a.width() / 90));
        for (double u = std::ceil(x_.lo / xs) * xs; u <= x_.hi; u += xs) {
            const double px = pixelX(u);
            p.drawLine(QPointF(px, a.top()), QPointF(px, a.bottom()));
        }
    }

    // zero line a little stronger
    if (v.y.lo < 0.0 && v.y.hi > 0.0 && !plain) {
        p.setPen(QPen(Theme::axis(), 1));
        const double py = pixelY(k, 0.0);
        p.drawLine(QPointF(a.left(), py), QPointF(a.right(), py));
    }

    p.save();
    p.setClipRect(a.adjusted(-1, -1, 1, 1));
    for (int i = 0; i < panel.curves.size(); ++i) drawCurve(p, panel.curves[i], k, ascending_[k][i]);
    p.restore();

    p.setPen(QPen(Theme::axis(), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(a);
}

void PlotWidget::drawCurve(QPainter& p, const Curve& c, int pane, bool sorted) {
    const int n = c.x.size();
    if (n == 0) return;
    const QRect& a = panes_[pane].area;
    int i0 = 0, i1 = n;
    if (sorted) {
        const double lo = fromX(x_.lo), hi = fromX(x_.hi);
        i0 = std::max(0, static_cast<int>(std::lower_bound(c.x.begin(), c.x.end(), lo) - c.x.begin()) - 1);
        i1 = std::min(n, static_cast<int>(std::upper_bound(c.x.begin(), c.x.end(), hi) - c.x.begin()) + 1);
    }
    if (c.markers) {
        // the points: a cloud (translucent, many may overlap) or the values of a sweep
        const bool cloud = c.markers == 1;
        QColor fill = c.color();
        if (cloud) fill.setAlphaF(0.55f);
        if (c.markers == 2) {
            p.setPen(QPen(c.color(), 2.0, penStyle(c.dash), Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            QPainterPath line;
            bool open = false;
            for (int k = i0; k < i1; ++k) {
                const double u = toX(c.x[k]);
                if (!std::isfinite(u) || !std::isfinite(c.y[k])) {
                    open = false;
                    continue;
                }
                const QPointF q(pixelX(u), std::clamp(pixelY(pane, c.y[k]), a.top() - 1e4, a.bottom() + 1e4));
                if (open) line.lineTo(q);
                else line.moveTo(q);
                open = true;
            }
            p.drawPath(line);
        }
        p.setPen(cloud ? QPen(Qt::NoPen) : QPen(Theme::surface(), 2.0));  // a ring of the background
        p.setBrush(fill);
        const double r = cloud ? 3.0 : 4.0;
        for (int k = i0; k < i1; ++k) {
            const double u = toX(c.x[k]);
            if (!std::isfinite(u) || !std::isfinite(c.y[k])) continue;
            const QPointF q(pixelX(u), pixelY(pane, c.y[k]));
            if (a.adjusted(-6, -6, 6, 6).contains(q.toPoint())) p.drawEllipse(q, r, r);
        }
        return;
    }
    QPen pen(c.color(), 2.0, penStyle(c.dash), Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    // Many points per pixel column: first, minimum, maximum and last of each
    // column keep the shape of the curve, its peaks included.
    QPainterPath path;
    bool open = false;
    const auto add = [&](double px, double py) {
        if (!open) path.moveTo(px, py);
        else path.lineTo(px, py);
        open = true;
    };
    const bool reduce = (i1 - i0) > 4 * a.width();
    int column = std::numeric_limits<int>::min();
    double first = 0, last = 0, mn = 0, mx = 0, colPx = 0;
    bool have = false;
    const auto flush = [&]() {
        if (!have) return;
        add(colPx, first);
        add(colPx, mn);
        add(colPx, mx);
        add(colPx, last);
        have = false;
    };
    for (int k = i0; k < i1; ++k) {
        const double u = toX(c.x[k]);
        const double y = c.y[k];
        if (!std::isfinite(u) || !std::isfinite(y)) {
            flush();
            open = false;  // a gap
            continue;
        }
        const double px = pixelX(u);
        double py = pixelY(pane, y);
        py = std::clamp(py, a.top() - 1e4, a.bottom() + 1e4);  // far outside: keep the direction
        if (!reduce) {
            add(px, py);
            continue;
        }
        const int col = static_cast<int>(std::floor(px));
        if (col != column) {
            flush();
            column = col;
            colPx = px;
            first = mn = mx = last = py;
            have = true;
        } else {
            mn = std::min(mn, py);
            mx = std::max(mx, py);
            last = py;
        }
    }
    flush();
    p.drawPath(path);
}

void PlotWidget::drawXAxis(QPainter& p) {
    if (panes_.isEmpty()) return;
    const QRect& a = panes_.back().area;
    const QFontMetrics fm(font());
    p.setPen(Theme::textSecondary());
    const int y = a.bottom() + 4;
    // tick label centred below its line, but kept inside the widget
    const auto tick = [&](double px, const QString& text) {
        const int w = fm.horizontalAdvance(text);
        const double left = std::clamp(px - w / 2.0, 2.0, double(width() - w - 2));
        p.drawText(QRectF(left, y, w + 1, fm.height()), Qt::AlignLeft | Qt::AlignTop, text);
    };
    if (data_.logX) {
        for (double d = std::ceil(x_.lo); d <= x_.hi; d += 1.0) {
            tick(pixelX(d), formatSI(std::pow(10.0, d), data_.xUnit, 3));
        }
        if (x_.hi - x_.lo < 1.2) {  // less than a decade: label the steps 2 .. 9 as well
            for (double d = std::floor(x_.lo); d <= x_.hi; d += 1.0)
                for (int m = 2; m <= 9; ++m) {
                    const double u = d + std::log10(m);
                    if (u < x_.lo || u > x_.hi) continue;
                    tick(pixelX(u), formatSI(std::pow(10.0, u), data_.xUnit, 3));
                }
        }
    } else {
        const double step = niceStep(x_.hi - x_.lo, std::max(2, a.width() / 90));
        const int exponent = siExponent(std::max(std::abs(x_.lo), std::abs(x_.hi)));
        const int decimals = std::max(0, static_cast<int>(-std::floor(std::log10(step) + 1e-9)) + exponent);
        for (double u = std::ceil(x_.lo / step) * step; u <= x_.hi; u += step) {
            tick(pixelX(u), formatWithExponent(u, exponent, decimals, data_.xUnit));
        }
    }
    p.drawText(QRect(a.left(), y + fm.height() + 2, a.width(), fm.height()), Qt::AlignHCenter | Qt::AlignTop,
               data_.xLabel);
}

void PlotWidget::drawLegend(QPainter& p) {
    const QFontMetrics fm(font());
    const int lineHeight = fm.height() + 4;
    int x = kLeft, y = 4;
    QStringList seen;
    bool sweep = false;
    QString firstRun, lastRun;
    for (const Panel& panel : data_.panels)
        for (const Curve& c : panel.curves) {
            if (c.runs > 1) {
                sweep = true;
                if (c.run == 0) firstRun = c.group;
                if (c.run == c.runs - 1) lastRun = c.group;
            }
            if (seen.contains(c.name)) continue;
            seen.push_back(c.name);
            const int w = 28 + fm.horizontalAdvance(c.name) + 16;
            if (x + w > width() - kRight && x > kLeft) {
                x = kLeft;
                y += lineHeight;
            }
            // in a sweep the colour stands for the run: the sample shows the line style only
            const QColor color = c.runs > 1 ? Theme::textSecondary() : c.color();
            if (c.markers != 1) {
                p.setPen(QPen(color, 2.0, penStyle(c.dash), Qt::RoundCap));
                p.drawLine(QPointF(x, y + lineHeight / 2.0), QPointF(x + 22, y + lineHeight / 2.0));
            }
            if (c.markers) {
                p.setPen(c.markers == 2 ? QPen(Theme::surface(), 2.0) : QPen(Qt::NoPen));
                p.setBrush(color);
                p.drawEllipse(QPointF(x + 11, y + lineHeight / 2.0), 4.0, 4.0);
            }
            p.setPen(Theme::textPrimary());
            p.drawText(QRect(x + 28, y, w - 28, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, c.name);
            x += w;
        }
    if (sweep) {
        y += lineHeight;
        x = kLeft;
        p.setPen(Theme::textPrimary());
        const int w1 = fm.horizontalAdvance(firstRun);
        p.drawText(QRect(x, y, w1 + 4, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, firstRun);
        x += w1 + 8;
        QLinearGradient g(x, 0, x + 80, 0);
        for (int k = 0; k <= 8; ++k) g.setColorAt(k / 8.0, Theme::sequential(k, 9));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(x, y + lineHeight / 2.0 - 3, 80, 6), 3, 3);
        x += 88;
        p.setPen(Theme::textPrimary());
        p.drawText(QRect(x, y, width() - x, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, lastRun);
    }
}

void PlotWidget::drawCrosshair(QPainter& p) {
    const int k = paneAt(mouse_);
    if (k < 0 || !panes_[k].area.contains(mouse_)) return;
    const QFontMetrics fm(font());
    const double u = axisX(mouse_.x());
    const double x = fromX(u);

    p.setPen(QPen(Theme::cursor(), 1, Qt::DashLine));
    for (const PaneView& v : panes_) p.drawLine(QPointF(mouse_.x(), v.area.top()), QPointF(mouse_.x(), v.area.bottom()));

    // values of the curves of this panel, with a dot on each
    struct Entry {
        QColor color;
        int dash;
        QString text;
        double y;
        double px;  // where the dot goes
    };
    QVector<Entry> entries;
    // a cloud of points: the point nearest to the pointer, with its x
    double bestDist = 12.0;
    const Curve* bestCurve = nullptr;
    int bestIndex = -1;
    for (const Curve& c : data_.panels[k].curves) {
        if (c.markers != 1) continue;
        for (int i = 0; i < c.x.size(); ++i) {
            const double d = std::hypot(pixelX(toX(c.x[i])) - mouse_.x(), pixelY(k, c.y[i]) - mouse_.y());
            if (d < bestDist) {
                bestDist = d;
                bestCurve = &c;
                bestIndex = i;
            }
        }
    }
    QString head = formatSI(x, data_.xUnit, 6);
    if (bestCurve) {
        head = QStringLiteral("%1 = %2").arg(data_.xLabel, formatSI(bestCurve->x[bestIndex], data_.xUnit, 6));
        entries.push_back({bestCurve->color(), bestCurve->dash,
                           label(*bestCurve) + "  " + formatSI(bestCurve->y[bestIndex], data_.panels[k].unit, 5),
                           bestCurve->y[bestIndex], pixelX(toX(bestCurve->x[bestIndex]))});
    }
    for (const Curve& c : data_.panels[k].curves) {
        if (c.markers == 1) continue;
        const double y = interpolate(c, x);
        if (std::isnan(y)) continue;
        entries.push_back({c.color(), c.dash, label(c) + "  " + formatSI(y, data_.panels[k].unit, 5), y,
                           double(mouse_.x())});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.y > b.y; });
    for (const Entry& e : entries) {
        const double py = pixelY(k, e.y);
        if (py < panes_[k].area.top() || py > panes_[k].area.bottom()) continue;
        p.setPen(QPen(Theme::surface(), 2));
        p.setBrush(e.color);
        p.drawEllipse(QPointF(e.px, py), 4.0, 4.0);
    }
    if (bestCurve == nullptr && std::all_of(data_.panels[k].curves.begin(), data_.panels[k].curves.end(),
                                                                [](const Curve& c) { return c.markers == 1; }))
        return;  // a cloud and nothing near: no box

    const int maxRows = 12;
    QStringList lines;
    lines << head;
    for (int i = 0; i < std::min<int>(entries.size(), maxRows); ++i) lines << entries[i].text;
    if (entries.size() > maxRows) lines << tr("… %1 weitere").arg(entries.size() - maxRows);
    int w = 0;
    for (const QString& l : lines) w = std::max(w, fm.horizontalAdvance(l));
    const int lineHeight = fm.height() + 2;
    QRect box(0, 0, w + 34, lines.size() * lineHeight + 10);
    QPoint at = mouse_ + QPoint(14, 14);
    if (at.x() + box.width() > width() - 4) at.setX(mouse_.x() - 14 - box.width());
    if (at.y() + box.height() > height() - 4) at.setY(std::max(4, height() - 4 - box.height()));
    box.moveTopLeft(at);
    p.setPen(QPen(Theme::axis(), 1));
    p.setBrush(Theme::surface());
    p.drawRoundedRect(box, 6, 6);
    int y = box.top() + 5;
    p.setPen(Theme::textSecondary());
    p.drawText(QRect(box.left() + 10, y, w + 20, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, lines[0]);
    y += lineHeight;
    for (int i = 1; i < lines.size(); ++i) {
        if (i - 1 < entries.size() && i <= maxRows) {
            p.setPen(QPen(entries[i - 1].color, 2.0, penStyle(entries[i - 1].dash), Qt::RoundCap));
            p.drawLine(QPointF(box.left() + 10, y + lineHeight / 2.0), QPointF(box.left() + 22, y + lineHeight / 2.0));
        }
        p.setPen(Theme::textPrimary());
        p.drawText(QRect(box.left() + 28, y, w + 4, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, lines[i]);
        y += lineHeight;
    }
}

// --- interaction ------------------------------------------------------------

void PlotWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && paneAt(e->pos()) >= 0) {
        dragging_ = true;
        dragStart_ = e->pos();
        mouse_ = e->pos();
    }
}

void PlotWidget::mouseMoveEvent(QMouseEvent* e) {
    mouse_ = e->pos();
    hover_ = true;
    update();
}

void PlotWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (!dragging_ || e->button() != Qt::LeftButton) return;
    dragging_ = false;
    const int k = paneAt(dragStart_);
    const QPoint end = e->pos();
    if (k >= 0 && std::abs(end.x() - dragStart_.x()) > 5) {
        const double a = axisX(std::min(end.x(), dragStart_.x())), b = axisX(std::max(end.x(), dragStart_.x()));
        x_ = {a, b};
        if (std::abs(end.y() - dragStart_.y()) >= 6) {
            const double y0 = valueY(k, std::max(end.y(), dragStart_.y()));
            const double y1 = valueY(k, std::min(end.y(), dragStart_.y()));
            panes_[k].y = {y0, y1};
            panes_[k].autoY = false;
        }
        for (int i = 0; i < panes_.size(); ++i) autoscaleY(i);
    }
    update();
}

void PlotWidget::mouseDoubleClickEvent(QMouseEvent*) { fitAll(); }

void PlotWidget::wheelEvent(QWheelEvent* e) {
    if (panes_.isEmpty()) return;
    const QPoint pd = e->pixelDelta(), ad = e->angleDelta();
    const bool trackpad = !pd.isNull();
    const double dx = trackpad ? pd.x() : ad.x() / 4.0;
    const double dy = trackpad ? pd.y() : ad.y() / 4.0;
    const QPointF pos = e->position();
    if (std::abs(dx) > std::abs(dy)) {
        const double shift = -dx / std::max(panes_[0].area.width(), 1) * (x_.hi - x_.lo);
        x_ = {x_.lo + shift, x_.hi + shift};
        for (int k = 0; k < panes_.size(); ++k) autoscaleY(k);
        update();
    } else if (dy != 0.0) {
        const double factor = std::pow(0.995, dy);
        const int k = paneAt(pos.toPoint());
        if ((e->modifiers() & Qt::ControlModifier) && k >= 0) zoomY(k, factor, pos.y());
        else zoomX(factor, pos.x());
    }
    e->accept();
}

bool PlotWidget::event(QEvent* e) {
    if (e->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture && !panes_.isEmpty()) {
            zoomX(1.0 / (1.0 + g->value()), mapFromGlobal(g->globalPosition().toPoint()).x());
            return true;
        }
    }
    if (e->type() == QEvent::Resize) layoutPanes();
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange ||
        e->type() == QEvent::ThemeChange)
        update();
    return QWidget::event(e);
}

void PlotWidget::leaveEvent(QEvent*) {
    hover_ = false;
    update();
}

void PlotWidget::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    menu.addAction(tr("Alles zeigen"), this, &PlotWidget::fitAll);
    menu.addAction(tr("Als Bild speichern …"), this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, tr("Bild speichern"), "plot.png",
                                                          tr("PNG-Bild (*.png)"));
        if (!path.isEmpty()) saveImage(path);
    });
    hover_ = false;
    menu.exec(e->globalPos());
}
