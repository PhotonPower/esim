#include "Theme.hpp"

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <algorithm>

namespace Theme {

bool dark() {
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown) return scheme == Qt::ColorScheme::Dark;
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QColor surface() { return dark() ? QColor("#1a1a19") : QColor("#fcfcfb"); }
QColor textPrimary() { return dark() ? QColor("#f0efec") : QColor("#0b0b0b"); }
QColor textSecondary() { return dark() ? QColor("#c3c2b7") : QColor("#52514e"); }
QColor grid() { return dark() ? QColor("#2e2e2c") : QColor("#ebeae6"); }
QColor axis() { return dark() ? QColor("#6b6a64") : QColor("#a3a29b"); }
QColor cursor() { return dark() ? QColor("#8f8e86") : QColor("#7a7973"); }

QColor series(int k) {
    static const char* light[kSeries] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100",
                                         "#e87ba4", "#008300", "#6250d6", "#e34948"};
    static const char* darkSteps[kSeries] = {"#3987e5", "#d95926", "#199e70", "#c98500",
                                             "#d55181", "#0ca30c", "#9085e9", "#e66767"};
    k = std::clamp(k, 0, kSeries - 1);
    return QColor(dark() ? darkSteps[k] : light[k]);
}

QColor sequential(int k, int count) {
    // Blue ramp; the end nearest the surface stays visible as a thin line.
    static const char* light[] = {"#6da7ec", "#5598e7", "#3987e5", "#2a78d6", "#256abf",
                                  "#1c5cab", "#184f95", "#104281", "#0d366b"};
    static const char* darkSteps[] = {"#1c5cab", "#256abf", "#2a78d6", "#3987e5", "#5598e7",
                                      "#6da7ec", "#86b6ef", "#9ec5f4", "#b7d3f6"};
    constexpr int n = 9;
    const double f = count > 1 ? static_cast<double>(k) / (count - 1) : 1.0;
    // interpolate between the steps, so that any number of runs gets its own shade
    const double pos = f * (n - 1);
    const int i = std::min(static_cast<int>(pos), n - 2);
    const double t = pos - i;
    const char** ramp = dark() ? darkSteps : light;
    const QColor a(ramp[i]), b(ramp[i + 1]);
    return QColor::fromRgbF(static_cast<float>(a.redF() + t * (b.redF() - a.redF())),
                            static_cast<float>(a.greenF() + t * (b.greenF() - a.greenF())),
                            static_cast<float>(a.blueF() + t * (b.blueF() - a.blueF())));
}

}  // namespace Theme
