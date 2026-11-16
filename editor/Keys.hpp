#pragma once

#include <QString>

// Keys as the platform names them in texts: "⌘D" on macOS, "Strg+D" on
// Windows and Linux (where Qt's Ctrl is the Control key).
namespace Keys {

#ifdef Q_OS_MACOS
inline QString cmd(const QString& key) { return QStringLiteral("⌘") + key; }
inline QString alt() { return QStringLiteral("⌥"); }
inline QString erase() { return QStringLiteral("⌫"); }
#else
inline QString cmd(const QString& key) { return QStringLiteral("Strg+") + key; }
inline QString alt() { return QStringLiteral("Alt"); }
inline QString erase() { return QStringLiteral("Entf"); }
#endif

}  // namespace Keys
