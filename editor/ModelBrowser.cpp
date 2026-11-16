#include "ModelBrowser.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString typeTitle(const QString& type) {
    static const QHash<QString, QString> titles = {
        {"d", QObject::tr("Diode")},       {"npn", QObject::tr("NPN")},       {"pnp", QObject::tr("PNP")},
        {"nmos", QObject::tr("N-MOSFET")}, {"pmos", QObject::tr("P-MOSFET")}, {"njf", QObject::tr("N-JFET")},
        {"pjf", QObject::tr("P-JFET")}};
    return titles.value(type, type);
}

QString keyOf(const LibraryModel& m) { return m.source + '\n' + m.name; }

}  // namespace

ModelBrowser::ModelBrowser(ModelLibrary* library, const QString& examplesDir, char kind, bool forComponent,
                           QWidget* parent)
    : QDialog(parent), library_(library), examplesDir_(examplesDir) {
    setWindowTitle(tr("Modellbibliothek"));
    resize(900, 560);

    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Suchen: Name, Beschreibung, Quelle"));
    search_->setClearButtonEnabled(true);
    kind_ = new QComboBox;
    kind_->addItem(tr("Alle Arten"), 0);
    kind_->addItem(tr("Dioden"), int('d'));
    kind_->addItem(tr("Bipolartransistoren"), int('q'));
    kind_->addItem(tr("MOSFETs"), int('m'));
    kind_->addItem(tr("JFETs"), int('j'));
    kind_->setCurrentIndex(std::max(0, kind_->findData(int(kind))));
    auto* libraries = new QPushButton(tr("Bibliotheken …"));
    auto* top = new QHBoxLayout;
    top->addWidget(search_, 1);
    top->addWidget(kind_);
    top->addWidget(libraries);

    list_ = new QTreeWidget;
    list_->setHeaderLabels({tr("Name"), tr("Art"), tr("Beschreibung"), tr("Quelle")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setSortingEnabled(true);
    list_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    card_ = new QPlainTextEdit;
    card_->setReadOnly(true);
    card_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    card_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(list_);
    split->addWidget(card_);
    split->setSizes({340, 180});

    problems_ = new QLabel;
    problems_->setWordWrap(true);
    problems_->hide();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    if (forComponent) {
        use_ = buttons->addButton(tr("Verwenden"), QDialogButtonBox::AcceptRole);
        use_->setDefault(true);
    } else {
        place_ = buttons->addButton(tr("Im Plan platzieren"), QDialogButtonBox::AcceptRole);
        add_ = buttons->addButton(tr("In die Datei übernehmen"), QDialogButtonBox::AcceptRole);
        place_->setDefault(true);
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::clicked, this, [this](QAbstractButton* b) {
        if (!chosen()) return;
        if (b == use_) action_ = Use;
        else if (b == place_) action_ = Place;
        else if (b == add_) action_ = AddToFile;
        else return;
        accept();
    });

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(split, 1);
    layout->addWidget(problems_);
    layout->addWidget(buttons);

    connect(search_, &QLineEdit::textChanged, this, &ModelBrowser::fill);
    connect(kind_, &QComboBox::currentIndexChanged, this, &ModelBrowser::fill);
    connect(list_, &QTreeWidget::currentItemChanged, this, &ModelBrowser::showCurrent);
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this, forComponent]() {
        if (!chosen()) return;
        action_ = forComponent ? Use : Place;
        accept();
    });
    connect(libraries, &QPushButton::clicked, this, &ModelBrowser::manageLibraries);
    fill();
    search_->setFocus();
}

const LibraryModel* ModelBrowser::chosen() const {
    for (const LibraryModel& m : library_->models())
        if (keyOf(m) == chosenKey_) return &m;
    return nullptr;
}

void ModelBrowser::fill() {
    const QString text = search_->text().trimmed();
    const char kind = char(kind_->currentData().toInt());
    const QString previous = chosenKey_;
    list_->setSortingEnabled(false);
    list_->clear();
    QTreeWidgetItem* select = nullptr;
    for (const LibraryModel& m : library_->models()) {
        if (kind && m.kind() != kind) continue;
        const QString haystack = m.name + ' ' + m.description + ' ' + m.source + ' ' + typeTitle(m.type);
        if (!text.isEmpty() && !haystack.contains(text, Qt::CaseInsensitive)) continue;
        auto* item = new QTreeWidgetItem(list_, {m.name, typeTitle(m.type), m.title(), m.source});
        item->setData(0, Qt::UserRole, keyOf(m));
        item->setToolTip(3, m.source);
        if (keyOf(m) == previous || !select) select = item;
    }
    list_->setSortingEnabled(true);
    list_->sortByColumn(-1, Qt::AscendingOrder);  // the order of the libraries
    for (int c = 0; c < 2; ++c) list_->resizeColumnToContents(c);
    list_->setCurrentItem(select);
    showCurrent();
    const QStringList problems = library_->problems();
    problems_->setText(tr("Nicht lesbar: %1").arg(problems.join("; ")));
    problems_->setVisible(!problems.isEmpty());
}

void ModelBrowser::showCurrent() {
    QTreeWidgetItem* item = list_->currentItem();
    chosenKey_ = item ? item->data(0, Qt::UserRole).toString() : QString();
    const LibraryModel* m = chosen();
    for (QPushButton* b : {place_, add_, use_})
        if (b) b->setEnabled(m != nullptr);
    if (!m) {
        card_->setPlainText(tr("Kein Modell gefunden."));
        return;
    }
    QString text;
    if (!m->description.isEmpty()) text += m->description + "\n\n";
    text += m->text + "\n\n" + tr("Quelle: %1").arg(m->source);
    card_->setPlainText(text);
}

void ModelBrowser::manageLibraries() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Eigene Bibliotheken"));
    dialog.resize(600, 320);
    auto* hint = new QLabel(tr("SPICE-Dateien mit .model-Karten (z. B. die Modelle der Hersteller) oder "
                               "esim-Dateien mit einem Abschnitt \"models\"; ein Ordner bringt alle solchen "
                               "Dateien darin mit."));
    hint->setWordWrap(true);
    auto* paths = new QListWidget;
    paths->addItems(QSettings().value("library/paths").toStringList());
    auto* addFile = new QPushButton(tr("Datei …"));
    auto* addDir = new QPushButton(tr("Ordner …"));
    auto* remove = new QPushButton(tr("Entfernen"));
    auto* side = new QVBoxLayout;
    side->addWidget(addFile);
    side->addWidget(addDir);
    side->addWidget(remove);
    side->addStretch(1);
    auto* middle = new QHBoxLayout;
    middle->addWidget(paths, 1);
    middle->addLayout(side);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(hint);
    layout->addLayout(middle, 1);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    connect(addFile, &QPushButton::clicked, &dialog, [&]() {
        const QStringList files = QFileDialog::getOpenFileNames(
            &dialog, tr("Bibliothek wählen"), QString(),
            tr("Modelle (*.lib *.mod *.txt *.cir *.sp *.json);;Alle Dateien (*)"));
        paths->addItems(files);
    });
    connect(addDir, &QPushButton::clicked, &dialog, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(&dialog, tr("Ordner mit Bibliotheken wählen"));
        if (!dir.isEmpty()) paths->addItem(dir);
    });
    connect(remove, &QPushButton::clicked, &dialog, [&]() { delete paths->currentItem(); });
    dialog.exec();
    QStringList list;
    for (int k = 0; k < paths->count(); ++k) list << paths->item(k)->text();
    list.removeDuplicates();
    QSettings().setValue("library/paths", list);
    library_->load(list, examplesDir_);
    fill();
}
