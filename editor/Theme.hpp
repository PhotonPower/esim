#pragma once

#include <QColor>

// Colours of the plots and the schematic, for the light and the dark
// appearance of the system. The categorical colours are the validated
// eight-hue palette (fixed order, separable with colour vision deficiencies
// between neighbours); each appearance has its own steps.
namespace Theme {

constexpr int kSeries = 8;

bool dark();  // the application currently shows the dark appearance

QColor surface();        // plot background
QColor textPrimary();
QColor textSecondary();
QColor grid();           // recessive grid lines
QColor axis();           // frame and tick marks
QColor cursor();         // crosshair

QColor series(int k);                // categorical slot k (0 .. kSeries-1)
QColor sequential(int k, int count); // step k of count on a light-to-dark blue scale

}  // namespace Theme
