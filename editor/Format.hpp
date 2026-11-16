#pragma once

#include <QString>

// Numbers with SI prefixes, as the input format writes them: 4.7k, 100n.

// "1.5 mV" (digits: significant digits; unit may be empty: "1.5m").
QString formatSI(double value, const QString& unit = QString(), int digits = 4);

// Exponent (multiple of 3) whose prefix suits numbers of magnitude `scale`.
int siExponent(double scale);

// value / 10^exponent as plain number with the given decimals, followed by
// the prefix and unit: used for the tick labels of one axis, which share a prefix.
QString formatWithExponent(double value, int exponent, int decimals, const QString& unit);
