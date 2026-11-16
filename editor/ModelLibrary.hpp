#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Device models to choose from: the standard library of the editor, the
// models of the examples, and libraries of the user (SPICE files with
// ".model" cards as manufacturers publish them, or esim JSON files with a
// "models" section). A chosen model is copied into the "models" section of
// the file, so that the file stays complete on its own.
struct LibraryModel {
    QString name;         // as in the card or the JSON key
    QString type;         // lower case: d, npn, pnp, nmos, pmos, njf, pjf
    QString value;        // JSON for the "models" section: a card (string or lines) or an object
    QString text;         // shown to the user: the card, or the JSON
    QString description;  // the comment above the card (first line: short title)
    QString source;       // file it comes from; ":/library/..." for the standard library

    QString title() const;  // first line of the description, without the leading name
    char kind() const;      // component kind it fits: 'd', 'q', 'm', 'j' (0: none)
};

// Reads the ".model" cards of a SPICE text (continuation lines with "+",
// comments with "*"); models of types esim does not know are skipped.
QVector<LibraryModel> parseSpiceLibrary(const QString& text, const QString& source);

// Reads the "models" section of an esim JSON file.
QVector<LibraryModel> parseJsonLibrary(const QString& text, const QString& source);

class ModelLibrary {
public:
    // The standard library, the examples (if found) and the given files or
    // folders (each folder: its *.lib, *.mod, *.txt, *.cir, *.sp, *.json).
    void load(const QStringList& paths, const QString& examplesDir);
    const QVector<LibraryModel>& models() const { return models_; }
    const QStringList& problems() const { return problems_; }  // files that could not be read
    const LibraryModel* find(const QString& name) const;       // first match, case-insensitive

private:
    void loadFile(const QString& path, const QString& label);
    QVector<LibraryModel> models_;
    QStringList problems_;
};

// Names of the models in the file's "models" section.
QStringList fileModels(const QString& text);

// The text with the model added to "models" (created if missing). If the
// file has a model of that name already, the text stays as it is.
QString addModel(const QString& text, const LibraryModel& m);
