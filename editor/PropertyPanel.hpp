#pragma once

#include <QStringList>
#include <QWidget>

#include "Schematic.hpp"

class ModelLibrary;
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
    // Models offered for the "model" field: those of the file and of the library.
    void setModels(const QStringList& fileModels, const ModelLibrary* library);
    void focusFirstField();
    bool editing() const;  // a field has the keyboard focus

signals:
    void memberChanged(int index, const QString& key, const QString& json);  // json empty: remove
    void renameRequested(int index, const QString& name);
    void showInText(int index);
    void browseModels(int index, char kind);  // the "…" button of the model field

private:
    QLineEdit* addField(const QString& key, const QString& value, bool present);

    QLabel* title_;
    QLabel* message_;
    QWidget* formHost_;
    QFormLayout* form_;
    QLineEdit* name_ = nullptr;
    QLineEdit* firstField_ = nullptr;
    QStringList fileModels_;
    const ModelLibrary* library_ = nullptr;
    char kind_ = 0;
    int index_ = -1;
    QString shownName_;
};

// JSON for the text of a field: valid JSON (number, true, object, "quoted")
// as it is, anything else as a string; empty for an empty field.
QString fieldJson(const QString& input);
