#pragma once

#include <QDialog>

#include "ModelLibrary.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

// Browses the model library: search, filter by kind, the card with its
// description and source; manages the user's library files and folders.
// The result says what to do with the chosen model.
class ModelBrowser : public QDialog {
    Q_OBJECT
public:
    enum Action { None, Place, AddToFile, Use };

    // kind: 0 all, else only models for that component kind ('d', 'q', 'm',
    // 'j'); forComponent: the dialog chooses the model of a component.
    ModelBrowser(ModelLibrary* library, const QString& examplesDir, char kind, bool forComponent,
                 QWidget* parent = nullptr);

    Action action() const { return action_; }
    const LibraryModel* chosen() const;

private:
    void fill();
    void showCurrent();
    void manageLibraries();

    ModelLibrary* library_;
    QString examplesDir_;
    QLineEdit* search_;
    QComboBox* kind_;
    QTreeWidget* list_;
    QPlainTextEdit* card_;
    QLabel* problems_;
    QPushButton* place_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* use_ = nullptr;
    Action action_ = None;
    QString chosenKey_;  // source + name
};
