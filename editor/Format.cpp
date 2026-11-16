#include "Format.hpp"

#include <algorithm>
#include <cmath>

namespace {

QString prefix(int exponent) {
    switch (exponent) {
        case -18: return "a";
        case -15: return "f";
        case -12: return "p";
        case -9: return "n";
        case -6: return QString::fromUtf8("µ");
        case -3: return "m";
        case 0: return "";
        case 3: return "k";
        case 6: return "M";
        case 9: return "G";
        case 12: return "T";
        default: return QString();
    }
}

QString join(const QString& number, int exponent, const QString& unit) {
    const QString p = prefix(exponent);
    if (unit.isEmpty() || unit == QString::fromUtf8("°")) return number + p + unit;
    return number + ' ' + p + unit;
}

}  // namespace

int siExponent(double scale) {
    scale = std::abs(scale);
    if (!(scale > 0.0) || !std::isfinite(scale)) return 0;
    const int e = static_cast<int>(std::floor(std::log10(scale) / 3.0)) * 3;
    return std::clamp(e, -18, 12);
}

QString formatSI(double value, const QString& unit, int digits) {
    if (std::isnan(value)) return QStringLiteral("–");
    if (std::isinf(value)) return value > 0 ? QStringLiteral("∞") : QStringLiteral("−∞");
    if (value == 0.0) return join("0", 0, unit);
    // dB and degrees carry no prefix
    if (unit == "dB" || unit == QString::fromUtf8("°"))
        return join(QString::number(value, 'g', digits).replace('-', QString::fromUtf8("−")), 0, unit);
    int e = siExponent(value);
    double m = value / std::pow(10.0, e);
    // rounding may carry over to the next prefix (999.97 -> 1000)
    if (std::abs(QString::number(m, 'g', digits).toDouble()) >= 1000.0 && e < 12) {
        e += 3;
        m = value / std::pow(10.0, e);
    }
    QString number = QString::number(m, 'g', digits);
    if (number.contains('e')) number = QString::number(m, 'f', 0);
    return join(number.replace('-', QString::fromUtf8("−")), e, unit);
}

QString formatWithExponent(double value, int exponent, int decimals, const QString& unit) {
    double m = value / std::pow(10.0, exponent);
    if (std::abs(m) < 0.5 * std::pow(10.0, -decimals)) m = 0.0;  // no "-0.0"
    QString number = QString::number(m, 'f', std::max(decimals, 0));
    return join(number.replace('-', QString::fromUtf8("−")), exponent, unit);
}
