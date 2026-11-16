#pragma once

#include <QMap>
#include <QStringList>
#include <QWidget>

class QLineEdit;
class QListWidget;
class QListWidgetItem;

// The list of parts for the schematic, with a search field: a click (or
// Enter in the search field) starts placing the part. Besides components
// it offers wire, ground and net label, and the subcircuits of the file.
class PartPalette : public QWidget {
    Q_OBJECT
public:
    explicit PartPalette(QWidget* parent = nullptr);

    void setSubcircuits(const QMap<QString, QStringList>& subcircuits);
    void focusSearch();

signals:
    // a template id (see partTemplates()), "sub:<name>", "wire", "ground" or "label"
    void partChosen(const QString& id);

protected:
    void changeEvent(QEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void rebuild();
    void choose(QListWidgetItem* item);

    QLineEdit* search_;
    QListWidget* list_;
    QMap<QString, QStringList> subcircuits_;
};
