#pragma once

#include <QStringList>
#include <QWidget>

#include "Schematic.hpp"

class QFormLayout;
class QLabel;
class QLineEdit;

// The parameters of the component selected in the schematic or the text,
// as a form. Every field is one member of the component's JSON object;
// a change replaces just that member in the text (strings are quoted
// automatically: "1k" stays a string, 1e3 becomes a number).
class PropertyPanel : public QWidget {
    Q_OBJECT
public:
    explicit PropertyPanel(QWidget* parent = nullptr);

    // c == nullptr: nothing (or several things) selected, `message` says which.
    void showComponent(const QString& text, const SComponent* c, const QString& message = QString());
    void setModelNames(const QStringList& names);
    void focusFirstField();
    bool editing() const;  // a field has the keyboard focus

signals:
    void memberChanged(int index, const QString& key, const QString& json);  // json empty: remove
    void renameRequested(int index, const QString& name);
    void showInText(int index);

private:
    QLineEdit* addField(const QString& key, const QString& value, bool present);

    QLabel* title_;
    QLabel* message_;
    QWidget* formHost_;
    QFormLayout* form_;
    QLineEdit* name_ = nullptr;
    QLineEdit* firstField_ = nullptr;
    QStringList models_;
    int index_ = -1;
    QString shownName_;
};

// JSON for the text of a field: valid JSON (number, true, object, "quoted")
// as it is, anything else as a string; empty for an empty field.
QString fieldJson(const QString& input);
