#include "MainWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QRegularExpression>
#include <QToolButton>
#include <QVBoxLayout>
#include <QStatusBar>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>

#include "CodeEditor.hpp"
#include "JsonSpan.hpp"
#include "ModelBrowser.hpp"
#include "PartPalette.hpp"
#include "PlotWidget.hpp"
#include "PropertyPanel.hpp"
#include "ResultsPanel.hpp"
#include "SchematicView.hpp"
#include "Symbols.hpp"
#include "SimulationRun.hpp"
#include "Theme.hpp"

namespace {

constexpr int kMaxRecent = 10;

const char* kTemplate = R"json({
  "title": "Neue Schaltung",

  "components": [
    { "type": "V", "name": "V1", "nodes": ["in", "0"],
      "wave": { "type": "pulse", "v1": 0, "v2": 1, "delay": "1m", "rise": "1u", "fall": "1u", "width": "5m", "period": "10m" } },
    { "type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k" },
    { "type": "C", "name": "C1", "nodes": ["out", "0"], "value": "1u" }
  ],

  "analyses": [
    { "type": "op" },
    { "type": "tran", "tstop": "20m" }
  ],

  "outputs": ["v(in)", "v(out)"],

  "schematic": {
    "components": {
      "V1": { "at": [0, 0] },
      "R1": { "at": [4, -2], "rot": 270 },
      "C1": { "at": [8, 0] }
    },
    "wires": [
      [[0, -2], [2, -2]],
      [[6, -2], [10, -2]]
    ],
    "labels": [
      { "name": "in", "at": [1, -2], "rot": 270 },
      { "name": "out", "at": [10, -2] }
    ],
    "grounds": [
      { "at": [0, 2] },
      { "at": [8, 2] }
    ]
  }
}
)json";

QString examplesDir() {
    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/examples"),
        QDir(QCoreApplication::applicationDirPath()).filePath("examples"),
#ifdef ESIM_SOURCE_DIR
        QStringLiteral(ESIM_SOURCE_DIR "/examples"),
#endif
    };
    for (const QString& c : candidates)
        if (QFileInfo(c).isDir()) return QDir(c).canonicalPath();
    return QString();
}

QColor logColor(int kind) {
    if (kind == int(SimulationRun::LineKind::Error)) return Theme::dark() ? QColor("#e66767") : QColor("#c42f2f");
    if (kind == int(SimulationRun::LineKind::Warning)) return Theme::dark() ? QColor("#d9a03a") : QColor("#9a6400");
    return QColor();
}

// Simple drawn icons for the tool bar (Qt greys them out when disabled).
QIcon runIcon(bool stop) {
    QPixmap pm(QSize(32, 32) * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    if (stop) {
        p.setBrush(QColor("#e34948"));
        p.drawRoundedRect(QRectF(8, 8, 16, 16), 3, 3);
    } else {
        p.setBrush(QColor("#1baf7a"));
        QPainterPath path;
        path.moveTo(9, 6);
        path.lineTo(26, 16);
        path.lineTo(9, 26);
        path.closeSubpath();
        p.drawPath(path);
    }
    return QIcon(pm);
}

}  // namespace

MainWindow::MainWindow() {
    editor_ = new CodeEditor;
    schematic_ = new SchematicView;
    // above the drawing of a subcircuit: where we are, and the way back
    scopeBar_ = new QWidget;
    auto* up = new QToolButton;
    up->setText(tr("‹ Hauptschaltung"));
    up->setToolTip(tr("Zurück zur Hauptschaltung (⌘↑)"));
    up->setAutoRaise(true);
    scopeLabel_ = new QLabel;
    auto* bar = new QHBoxLayout(scopeBar_);
    bar->setContentsMargins(6, 2, 6, 2);
    bar->addWidget(up);
    bar->addWidget(scopeLabel_, 1);
    scopeBar_->hide();
    connect(up, &QToolButton::clicked, this, [this]() { enterScope(QString()); });
    planBox_ = new QWidget;
    auto* planLayout = new QVBoxLayout(planBox_);
    planLayout->setContentsMargins(0, 0, 0, 0);
    planLayout->setSpacing(0);
    planLayout->addWidget(scopeBar_);
    planLayout->addWidget(schematic_, 1);
    split_ = new QSplitter(Qt::Horizontal);
    split_->addWidget(planBox_);
    split_->addWidget(editor_);
    split_->setStretchFactor(0, 3);
    split_->setStretchFactor(1, 2);
    split_->setChildrenCollapsible(false);
    setCentralWidget(split_);
    run_ = new SimulationRun(this);
    checkTimer_ = new QTimer(this);
    checkTimer_->setSingleShot(true);
    checkTimer_->setInterval(400);
    schematicTimer_ = new QTimer(this);
    schematicTimer_->setSingleShot(true);
    schematicTimer_->setInterval(150);

    createDocks();
    createActions();

    status_ = new QLabel;
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setMaximumWidth(200);
    progress_->setTextVisible(false);
    progress_->hide();
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(progress_);

    connect(editor_->document(), &QTextDocument::contentsChanged, this, [this]() {
        setWindowModified(editor_->document()->isModified());
        scheduleCheck();
        if (!writing_) schematicTimer_->start();
    });
    connect(schematicTimer_, &QTimer::timeout, this, &MainWindow::refreshSchematic);
    connect(editor_, &QPlainTextEdit::cursorPositionChanged, this, &MainWindow::textCursorMoved);
    connect(schematic_, &SchematicView::edited, this, &MainWindow::applySchematic);
    connect(schematic_, &SchematicView::selectionChanged, this, &MainWindow::schematicSelectionChanged);
    connect(schematic_, &SchematicView::componentActivated, this, [this](const QString&) {
        propsDock_->show();
        propsDock_->raise();
        props_->focusFirstField();
    });
    connect(schematic_, &SchematicView::probe, this, &MainWindow::probe);
    connect(schematic_, &SchematicView::openSubcircuit, this, &MainWindow::enterScope);
    connect(schematic_, &SchematicView::leaveRequested, this, [this]() { enterScope(QString()); });
    connect(schematic_, &SchematicView::partChooserRequested, this, [this]() {
        paletteDock_->show();
        paletteDock_->raise();
        palette_->focusSearch();
    });
    connect(palette_, &PartPalette::partChosen, this, &MainWindow::choosePart);
    library_.load(QSettings().value("library/paths").toStringList(), examplesDir());
    palette_->setLibrary(&library_.models());
    connect(props_, &PropertyPanel::memberChanged, this, [this](int index, const QString& key, const QString& json) {
        QString text = editor_->toPlainText();
        if (key == "model") {
            JValue v;
            if (parseJsonc(json, v) && v.isString()) text = importModels(text, {v.string});
        }
        applyText(setComponentMember(text, index, key, json, scope_));
        refreshSchematic();
    });
    connect(props_, &PropertyPanel::renameRequested, this, &MainWindow::renameComponent);
    connect(props_, &PropertyPanel::browseModels, this, [this](int index, char kind) {
        ModelBrowser browser(&library_, examplesDir(), kind, true, this);
        const bool done = browser.exec() == QDialog::Accepted && browser.chosen();
        palette_->setLibrary(&library_.models());  // libraries may have changed
        if (!done) return;
        const QString name = browser.chosen()->name;
        applyText(setComponentMember(importModels(editor_->toPlainText(), {name}), index, "model", jsonQuote(name), scope_));
        refreshSchematic();
    });
    connect(props_, &PropertyPanel::showInText, this, [this](int index) { revealInText(index, true); });
    connect(editor_->document(), &QTextDocument::modificationChanged, this, &QWidget::setWindowModified);
    connect(checkTimer_, &QTimer::timeout, this, [this]() {
        checker_.check(editor_->toPlainText(), file_.isEmpty() ? QDir::currentPath() : QFileInfo(file_).absolutePath());
    });
    connect(&checker_, &Checker::checked, this, &MainWindow::showCheck);
    connect(run_, &SimulationRun::line, this, [this](const QString& text, SimulationRun::LineKind kind) {
        appendLog(text, int(kind));
    });
    connect(run_, &SimulationRun::progress, this, [this](const QString& analysis, double f) {
        progress_->show();
        progress_->setValue(int(f * 1000));
        status_->setText(tr("Simulation läuft: %1 %2 %").arg(analysis).arg(int(f * 100)));
    });
    connect(run_, &SimulationRun::finished, this, &MainWindow::simulationFinished);

    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    if (!restoreState(settings.value("window/state").toByteArray())) resize(1440, 900);
    if (!split_->restoreState(settings.value("window/split").toByteArray())) split_->setSizes({600, 420});
    showPlan_->setChecked(settings.value("window/showPlan", true).toBool());
    showText_->setChecked(settings.value("window/showText", true).toBool());
    updateRecentMenu();
    newFile();
}

// --- menus and docks --------------------------------------------------------

void MainWindow::createDocks() {
    palette_ = new PartPalette;
    paletteDock_ = new QDockWidget(tr("Bauteile"), this);
    paletteDock_->setObjectName("parts");
    paletteDock_->setWidget(palette_);
    addDockWidget(Qt::LeftDockWidgetArea, paletteDock_);
    props_ = new PropertyPanel;
    propsDock_ = new QDockWidget(tr("Eigenschaften"), this);
    propsDock_->setObjectName("properties");
    propsDock_->setWidget(props_);
    addDockWidget(Qt::LeftDockWidgetArea, propsDock_);
    splitDockWidget(paletteDock_, propsDock_, Qt::Vertical);
    resizeDocks({paletteDock_}, {240}, Qt::Horizontal);

    results_ = new ResultsPanel;
    resultsDock_ = new QDockWidget(tr("Ergebnisse"), this);
    resultsDock_->setObjectName("results");
    resultsDock_->setWidget(results_);
    addDockWidget(Qt::BottomDockWidgetArea, resultsDock_);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(20000);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logDock_ = new QDockWidget(tr("Protokoll"), this);
    logDock_->setObjectName("log");
    logDock_->setWidget(log_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);

    problems_ = new QListWidget;
    problemsDock_ = new QDockWidget(tr("Probleme"), this);
    problemsDock_->setObjectName("problems");
    problemsDock_->setWidget(problems_);
    addDockWidget(Qt::BottomDockWidgetArea, problemsDock_);
    connect(problems_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const int line = item->data(Qt::UserRole).toInt();
        if (line >= 0) editor_->goToLine(line, item->data(Qt::UserRole + 1).toInt());
    });

    tabifyDockWidget(resultsDock_, logDock_);
    tabifyDockWidget(logDock_, problemsDock_);
    resultsDock_->raise();
    resizeDocks({resultsDock_}, {340}, Qt::Vertical);
}

void MainWindow::createActions() {
    QMenu* file = menuBar()->addMenu(tr("&Datei"));
    file->addAction(tr("&Neu"), QKeySequence::New, this, [this]() {
        if (maybeSave()) newFile();
    });
    file->addAction(tr("Ö&ffnen …"), QKeySequence::Open, this, &MainWindow::openDialog);
    recentMenu_ = file->addMenu(tr("Zuletzt geöffnet"));
    examplesMenu_ = file->addMenu(tr("Beispiele"));
    fillExamplesMenu();
    file->addSeparator();
    file->addAction(tr("&Sichern"), QKeySequence::Save, this, &MainWindow::save);
    file->addAction(tr("Sichern &unter …"), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    file->addSeparator();
    QAction* quit = file->addAction(tr("&Beenden"), QKeySequence::Quit, this, &QWidget::close);
    quit->setMenuRole(QAction::QuitRole);

    QMenu* edit = menuBar()->addMenu(tr("&Bearbeiten"));
    QAction* undo = edit->addAction(tr("&Widerrufen"), QKeySequence::Undo, editor_, &QPlainTextEdit::undo);
    QAction* redo = edit->addAction(tr("Wieder&holen"), QKeySequence::Redo, editor_, &QPlainTextEdit::redo);
    undo->setEnabled(false);
    redo->setEnabled(false);
    connect(editor_, &QPlainTextEdit::undoAvailable, undo, &QAction::setEnabled);
    connect(editor_, &QPlainTextEdit::redoAvailable, redo, &QAction::setEnabled);
    edit->addSeparator();
    edit->addAction(tr("&Ausschneiden"), QKeySequence::Cut, editor_, &QPlainTextEdit::cut);
    edit->addAction(tr("&Kopieren"), QKeySequence::Copy, editor_, &QPlainTextEdit::copy);
    edit->addAction(tr("E&infügen"), QKeySequence::Paste, editor_, &QPlainTextEdit::paste);
    edit->addAction(tr("Alles auswählen"), QKeySequence::SelectAll, editor_, &QPlainTextEdit::selectAll);
    edit->addSeparator();
    edit->addAction(tr("Kommentar ein/aus"), QKeySequence(Qt::CTRL | Qt::Key_Slash), editor_, &CodeEditor::toggleComment);
    edit->addSeparator();
    edit->addAction(tr("Modellbibliothek …"), QKeySequence(Qt::CTRL | Qt::Key_L), this, &MainWindow::openLibrary);

    QMenu* sim = menuBar()->addMenu(tr("&Simulation"));
    runAction_ = sim->addAction(tr("&Starten"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::runSimulation);
    stopAction_ = sim->addAction(tr("Sto&ppen"), QKeySequence(Qt::CTRL | Qt::Key_Period), this, &MainWindow::stopSimulation);
    stopAction_->setEnabled(false);

    QMenu* view = menuBar()->addMenu(tr("&Ansicht"));
    showPlan_ = view->addAction(tr("Schaltplan"));
    showText_ = view->addAction(tr("Text"));
    for (QAction* a : {showPlan_, showText_}) {
        a->setCheckable(true);
        a->setChecked(true);
    }
    connect(showPlan_, &QAction::toggled, this, [this](bool on) {
        if (!on && !showText_->isChecked()) showText_->setChecked(true);  // one of both stays
        planBox_->setVisible(on);
    });
    connect(showText_, &QAction::toggled, this, [this](bool on) {
        if (!on && !showPlan_->isChecked()) showPlan_->setChecked(true);
        editor_->setVisible(on);
    });
    view->addAction(tr("Schaltplan einpassen"), this, [this]() { schematic_->fitAll(); });
    view->addSeparator();
    view->addAction(paletteDock_->toggleViewAction());
    view->addAction(propsDock_->toggleViewAction());
    view->addAction(resultsDock_->toggleViewAction());
    view->addAction(logDock_->toggleViewAction());
    view->addAction(problemsDock_->toggleViewAction());
    view->addSeparator();
    view->addAction(tr("Kurven: alles zeigen"), QKeySequence(Qt::CTRL | Qt::Key_0), this, [this]() {
        results_->plot()->fitAll();
    });

    QMenu* help = menuBar()->addMenu(tr("&Hilfe"));
    QAction* about = help->addAction(tr("Über ESIM Editor"), this, [this]() {
        QMessageBox::about(this, tr("Über ESIM Editor"),
                           tr("<b>ESIM Editor</b><p>Eingabe, Simulation und Kurven für den Simulator esim.</p>"
                              "<p>MIT-Lizenz.</p>"));
    });
    about->setMenuRole(QAction::AboutRole);

    QToolBar* bar = addToolBar(tr("Werkzeuge"));
    bar->setObjectName("toolbar");
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    runAction_->setIcon(runIcon(false));
    stopAction_->setIcon(runIcon(true));
    runAction_->setToolTip(tr("Simulation starten (%1)").arg(runAction_->shortcut().toString(QKeySequence::NativeText)));
    stopAction_->setToolTip(tr("Simulation stoppen (%1)").arg(stopAction_->shortcut().toString(QKeySequence::NativeText)));
    bar->addAction(runAction_);
    bar->addAction(stopAction_);
}

void MainWindow::fillExamplesMenu() {
    examplesMenu_->clear();
    const QString dir = examplesDir();
    const QStringList files = dir.isEmpty() ? QStringList()
                                            : QDir(dir).entryList({"*.json"}, QDir::Files, QDir::Name);
    for (const QString& f : files) {
        const QString path = QDir(dir).filePath(f);
        examplesMenu_->addAction(QFileInfo(f).completeBaseName(), this, [this, path]() {
            if (maybeSave()) openFile(path);
        });
    }
    examplesMenu_->setEnabled(!files.isEmpty());
}

void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    const QStringList recent = QSettings().value("recent").toStringList();
    for (const QString& path : recent) {
        recentMenu_->addAction(QFileInfo(path).fileName(), this, [this, path]() {
            if (maybeSave()) openFile(path);
        })->setToolTip(path);
    }
    if (!recent.isEmpty()) {
        recentMenu_->addSeparator();
        recentMenu_->addAction(tr("Liste leeren"), this, [this]() {
            QSettings().remove("recent");
            updateRecentMenu();
        });
    }
    recentMenu_->setEnabled(!recent.isEmpty());
}

// --- files ------------------------------------------------------------------

void MainWindow::newFile() {
    editor_->setPlainText(QString::fromUtf8(kTemplate));
    editor_->document()->setModified(false);
    setCurrentFile(QString());
    loaded();
}

void MainWindow::loaded() {
    results_->clear();
    op_.clear();
    scope_.clear();
    scopeBar_->hide();
    palette_->setScope(QString());
    schematic_->setOperatingPoint({});
    schematic_->resetView();
    schematic_->selectComponents({});
    refreshSchematic();
    scheduleCheck();
}

void MainWindow::openDialog() {
    if (!maybeSave()) return;
    QSettings settings;
    const QString path = QFileDialog::getOpenFileName(this, tr("Netzliste öffnen"),
                                                      settings.value("lastDir", QDir::homePath()).toString(),
                                                      tr("Netzlisten (*.json);;Alle Dateien (*)"));
    if (!path.isEmpty()) openFile(path);
}

bool MainWindow::openFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Öffnen"), tr("%1 lässt sich nicht lesen:\n%2").arg(path, f.errorString()));
        return false;
    }
    editor_->setPlainText(QString::fromUtf8(f.readAll()));
    editor_->document()->setModified(false);
    setCurrentFile(QFileInfo(path).absoluteFilePath());
    log_->clear();
    loaded();
    return true;
}

bool MainWindow::save() { return file_.isEmpty() ? saveAs() : saveTo(file_); }

bool MainWindow::saveAs() {
    QSettings settings;
    const QString start = file_.isEmpty() ? QDir(settings.value("lastDir", QDir::homePath()).toString()).filePath("schaltung.json")
                                          : file_;
    QString path = QFileDialog::getSaveFileName(this, tr("Netzliste sichern"), start, tr("Netzlisten (*.json)"));
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty()) path += ".json";
    return saveTo(path);
}

bool MainWindow::saveTo(const QString& path) {
    QSaveFile f(path);  // written completely or not at all
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Sichern"), tr("%1 lässt sich nicht schreiben:\n%2").arg(path, f.errorString()));
        return false;
    }
    f.write(editor_->toPlainText().toUtf8());
    if (!f.commit()) {
        QMessageBox::warning(this, tr("Sichern"), tr("%1 lässt sich nicht schreiben:\n%2").arg(path, f.errorString()));
        return false;
    }
    editor_->document()->setModified(false);
    setCurrentFile(QFileInfo(path).absoluteFilePath());
    return true;
}

bool MainWindow::maybeSave() {
    if (!editor_->document()->isModified()) return true;
    const auto answer = QMessageBox::warning(
        this, tr("Änderungen sichern?"),
        tr("„%1“ wurde geändert. Sollen die Änderungen gesichert werden?")
            .arg(file_.isEmpty() ? tr("Unbenannt") : QFileInfo(file_).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

void MainWindow::setCurrentFile(const QString& path) {
    file_ = path;
    setWindowFilePath(path.isEmpty() ? tr("Unbenannt") : path);
    setWindowModified(editor_->document()->isModified());
    if (path.isEmpty()) return;
    QSettings settings;
    settings.setValue("lastDir", QFileInfo(path).absolutePath());
    QStringList recent = settings.value("recent").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecent) recent.removeLast();
    settings.setValue("recent", recent);
    updateRecentMenu();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    run_->stop();
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    settings.setValue("window/split", split_->saveState());
    settings.setValue("window/showPlan", showPlan_->isChecked());
    settings.setValue("window/showText", showText_->isChecked());
    e->accept();
}

// --- automation -------------------------------------------------------------

void MainWindow::automate(bool run, const QString& snapshot) {
    snapshot_ = snapshot;
    if (run) {
        QTimer::singleShot(0, this, &MainWindow::runSimulation);
    } else if (!snapshot.isEmpty()) {
        connect(&checker_, &Checker::checked, this, [this](const CheckResult& r) { finishAutomation(r.ok); });
    }
}

void MainWindow::finishAutomation(bool ok) {
    if (snapshot_.isEmpty()) return;
    const QString path = snapshot_;
    snapshot_.clear();
    // let the window lay itself out and paint once more
    QTimer::singleShot(300, this, [this, path, ok]() {
        const bool saved = grab().save(path);
        QCoreApplication::exit(ok && saved ? 0 : 1);
    });
}

// --- check while typing -----------------------------------------------------

void MainWindow::scheduleCheck() { checkTimer_->start(); }

void MainWindow::showCheck(const CheckResult& r) {
    lastCheck_ = r;
    updateProblems();
}

void MainWindow::updateProblems() {
    const CheckResult& r = lastCheck_;
    QVector<Diagnostic> all = r.diagnostics;
    all += schematicDiagnostics_;
    editor_->setDiagnostics(all);
    problems_->clear();
    int errors = 0, warnings = 0;
    for (const Diagnostic& d : all) {
        const bool error = d.severity == Diagnostic::Error;
        (error ? errors : warnings)++;
        QString text = d.message;
        if (d.line >= 0) text = tr("Zeile %1: %2").arg(d.line + 1).arg(d.message);
        auto* item = new QListWidgetItem(style()->standardIcon(error ? QStyle::SP_MessageBoxCritical
                                                                     : QStyle::SP_MessageBoxWarning),
                                         text, problems_);
        item->setData(Qt::UserRole, d.line);
        item->setData(Qt::UserRole + 1, d.column);
    }
    problemsDock_->setWindowTitle(errors + warnings ? tr("Probleme (%1)").arg(errors + warnings) : tr("Probleme"));
    if (run_->running()) return;
    if (errors) status_->setText(tr("%n Fehler", "", errors) + (warnings ? tr(", %n Warnungen", "", warnings) : QString()));
    else status_->setText(r.summary + (warnings ? tr(" · %n Warnungen", "", warnings) : QString()));
}

// --- simulation -------------------------------------------------------------

void MainWindow::runSimulation() {
    if (run_->running()) return;
    // the simulator reads the file: save first
    if (file_.isEmpty() || editor_->document()->isModified()) {
        if (!save()) return;
    }
    outDir_ = std::make_unique<QTemporaryDir>();
    if (!outDir_->isValid()) {
        appendLog(tr("Kein temporäres Verzeichnis für die Ergebnisse: %1").arg(outDir_->errorString()),
                  int(SimulationRun::LineKind::Error));
        return;
    }
    log_->clear();
    appendLog(tr("▶ %1").arg(QFileInfo(file_).fileName()), int(SimulationRun::LineKind::Output));
    clock_.start();
    if (!run_->start(file_, outDir_->path())) return;
    runAction_->setEnabled(false);
    stopAction_->setEnabled(true);
    progress_->setValue(0);
    progress_->show();
    status_->setText(tr("Simulation läuft …"));
}

void MainWindow::stopSimulation() { run_->stop(); }

void MainWindow::appendLog(const QString& text, int kind) {
    const QColor color = logColor(kind);
    if (!color.isValid()) {
        log_->appendPlainText(text);
        return;
    }
    QTextCharFormat format;
    format.setForeground(color);
    QTextCursor c(log_->document());
    c.movePosition(QTextCursor::End);
    if (!log_->document()->isEmpty()) c.insertBlock();
    c.insertText(text, format);
    log_->verticalScrollBar();
    log_->ensureCursorVisible();
}

void MainWindow::simulationFinished(bool ok, bool stopped) {
    runAction_->setEnabled(true);
    stopAction_->setEnabled(false);
    progress_->hide();
    const QString seconds = QString::number(clock_.elapsed() / 1000.0, 'f', 2);
    if (!snapshot_.isEmpty()) QTimer::singleShot(0, this, [this, ok, stopped]() { finishAutomation(ok && !stopped); });
    if (stopped) {
        status_->setText(tr("Simulation gestoppt"));
        appendLog(tr("■ gestoppt nach %1 s").arg(seconds), int(SimulationRun::LineKind::Warning));
        return;
    }
    if (!ok) {
        status_->setText(tr("Simulation fehlgeschlagen, siehe Protokoll"));
        logDock_->raise();
        logDock_->show();
        return;
    }
    appendLog(tr("✓ fertig in %1 s").arg(seconds), int(SimulationRun::LineKind::Output));
    QString error;
    if (!results_->load(run_->summaryPath(), error)) {
        appendLog(error, int(SimulationRun::LineKind::Error));
        logDock_->raise();
        return;
    }
    status_->setText(tr("Simulation fertig in %1 s").arg(seconds));
    op_ = results_->operatingPoint();
    updateOperatingPoint();
    resultsDock_->show();
    resultsDock_->raise();
}

// --- schematic and text -------------------------------------------------------

void MainWindow::refreshSchematic() {
    schematicTimer_->stop();
    const QString text = editor_->toPlainText();
    Schematic s;
    if (!readSchematic(text, s, nullptr, scope_)) {
        Schematic main;
        if (!scope_.isEmpty() && readSchematic(text, main)) {
            enterScope(QString());  // the subcircuit is gone (renamed or deleted in the text)
            return;
        }
        schematic_->setStale(true);
        return;
    }
    schematic_->setStale(false);
    schematic_->setSchematic(s);
    palette_->setSubcircuits(s.subcircuits);
    props_->setModels(s.modelTypes.keys(), &library_);

    // where drawing and text disagree
    schematicDiagnostics_.clear();
    const Connectivity& c = schematic_->connectivity();
    for (const auto& [k, pin] : c.conflicts) {
        const SComponent& comp = s.components[k];
        Diagnostic d;
        d.severity = Diagnostic::Warning;
        d.line = editor_->document()->findBlock(comp.begin).blockNumber();
        d.message = tr("Schaltplan: %1, Anschluss %2 liegt im Plan an „%3“, im Text an „%4“")
                        .arg(comp.name, symbolOf(comp).pins.value(pin).name, c.pinNodes[k][pin], comp.nodes[pin]);
        schematicDiagnostics_ << d;
    }
    for (const QString& w : c.warnings) {
        Diagnostic d;
        d.severity = Diagnostic::Warning;
        d.message = tr("Schaltplan: %1").arg(w);
        schematicDiagnostics_ << d;
    }
    updateProblems();
    showProperties();
}

void MainWindow::applyText(const QString& text) {
    const QString old = editor_->toPlainText();
    if (text == old) return;
    // replace only the part that differs: cursor, scroll position and the
    // undo history of the rest stay
    const int n = int(std::min(old.size(), text.size()));
    int prefix = 0;
    while (prefix < n && old[prefix] == text[prefix]) ++prefix;
    int suffix = 0;
    while (suffix < n - prefix && old[old.size() - 1 - suffix] == text[text.size() - 1 - suffix]) ++suffix;
    writing_ = true;
    QTextCursor c(editor_->document());
    c.beginEditBlock();
    c.setPosition(prefix);
    c.setPosition(int(old.size()) - suffix, QTextCursor::KeepAnchor);
    c.insertText(text.mid(prefix, text.size() - prefix - suffix));
    c.endEditBlock();
    writing_ = false;
}

void MainWindow::applySchematic(const Schematic& after) {
    // new components with a model or subcircuit of the library bring it along
    QStringList models;
    for (const SComponent& c : after.components) {
        if (c.index >= 0) continue;
        if (!c.model.isEmpty()) models << c.model;
        if (c.kind == 'x' && !c.subcircuit.isEmpty()) models << c.subcircuit;
    }
    applyText(writeSchematic(importModels(editor_->toPlainText(), models), after));
    refreshSchematic();
}

void MainWindow::showProperties() {
    const QStringList names = schematic_->selectedComponents();
    const Schematic& s = schematic_->schematic();
    const int k = names.size() == 1 ? s.find(names.first()) : -1;
    if (k >= 0) {
        if (!props_->editing()) props_->showComponent(editor_->toPlainText(), &s.components[k]);
        editor_->setHighlight(s.components[k].begin, s.components[k].end);
        return;
    }
    editor_->setHighlight(-1, -1);
    if (props_->editing()) return;
    props_->showComponent(QString(), nullptr,
                          names.isEmpty() ? tr("Ein Bauteil im Plan oder im Text auswählen.")
                                          : tr("%n Bauteile ausgewählt.", "", int(names.size())));
}

void MainWindow::revealInText(int index, bool focus) {
    for (const SComponent& c : schematic_->schematic().components) {
        if (c.index != index || c.begin < 0) continue;
        syncingCursor_ = true;
        QTextCursor cur(editor_->document());
        cur.setPosition(std::min(c.begin, editor_->document()->characterCount() - 1));
        editor_->setTextCursor(cur);
        editor_->ensureCursorVisible();
        syncingCursor_ = false;
        if (focus) {
            showText_->setChecked(true);
            editor_->setFocus();
        }
        return;
    }
}

void MainWindow::schematicSelectionChanged(const QStringList& names) {
    showProperties();
    if (names.size() != 1) return;
    const Schematic& s = schematic_->schematic();
    const int k = s.find(names.first());
    if (k >= 0) revealInText(s.components[k].index, false);
}

void MainWindow::textCursorMoved() {
    // not while the drawing moves the cursor or writes, and only when it is up to date
    if (syncingCursor_ || writing_ || schematicTimer_->isActive()) return;
    const int pos = editor_->textCursor().position();
    const auto select = [this, pos]() {
        for (const SComponent& c : schematic_->schematic().components) {
            if (c.begin < 0 || pos < c.begin || pos > c.end) continue;
            if (schematic_->selectedComponents() != QStringList{c.name}) {
                schematic_->selectComponents({c.name});
                showProperties();
            }
            return true;
        }
        return false;
    };
    if (select()) return;
    // a component of another level: the drawing goes there
    QString scope;
    int index = -1;
    if (locateComponent(editor_->toPlainText(), pos, scope, index) && scope != scope_) {
        enterScope(scope);
        select();
    }
}

void MainWindow::choosePart(const QString& id) {
    showPlan_->setChecked(true);
    const Schematic& s = schematic_->schematic();
    if (id == "wire") schematic_->startWire();
    else if (id == "ground") schematic_->startGround();
    else if (id == "label") schematic_->startLabel();
    else if (id == "port") schematic_->startPort();
    else if (id == "newsub") newSubcircuit();
    else if (id == "library") openLibrary();
    else if (id.startsWith("lib:")) {
        const int k = id.mid(4).toInt();
        if (k >= 0 && k < library_.models().size()) placeLibraryModel(library_.models()[k]);
    }
    else if (id.startsWith("sub:")) schematic_->startPlacing(makeSubcircuit(id.mid(4), s));
    else
        for (const PartTemplate& t : partTemplates())
            if (t.id == id) schematic_->startPlacing(makeComponent(t, s));
}

void MainWindow::renameComponent(int index, const QString& name) {
    Schematic after = schematic_->schematic();
    int k = -1;
    for (int i = 0; i < after.components.size(); ++i) {
        if (after.components[i].index == index) k = i;
        else if (after.components[i].name.compare(name, Qt::CaseInsensitive) == 0) {
            status_->setText(tr("Den Namen „%1“ trägt schon ein anderes Bauteil").arg(name));
            showProperties();
            return;
        }
    }
    if (k < 0) return;
    if (!after.hasSection) {
        // no drawing in the file yet: only the name changes
        applyText(setComponentMember(editor_->toPlainText(), index, "name", jsonQuote(name), scope_));
        schematic_->selectComponents({});
        refreshSchematic();
    } else {
        after.components[k].name = name;
        applySchematic(after);
    }
    schematic_->selectComponents({name});
    showProperties();
}

void MainWindow::probe(const QString& probed) {
    QString quantity = probed;
    if (!scope_.isEmpty()) {
        const QVector<Instance> instances = instancesOf(editor_->toPlainText(), scope_);
        if (instances.isEmpty()) {
            status_->setText(tr("„%1“ wird in der Hauptschaltung nicht verwendet").arg(scope_));
            return;
        }
        int pick = 0;
        if (instances.size() > 1) {
            QStringList names;
            for (const Instance& in : instances) names << in.name;
            bool ok = false;
            const QString name = QInputDialog::getItem(this, tr("Instanz"), tr("Kurve aus welcher Instanz von „%1“?").arg(scope_),
                                                       names, 0, false, &ok);
            if (!ok) return;
            pick = int(names.indexOf(name));
        }
        quantity = instanceQuantity(probed, instances[pick]);
    }
    if (results_->empty()) {
        status_->setText(tr("Für Kurven zuerst simulieren (%1)").arg(runAction_->shortcut().toString(QKeySequence::NativeText)));
        return;
    }
    if (results_->probe(quantity)) {
        resultsDock_->show();
        resultsDock_->raise();
        status_->setText(tr("%1 gezeigt").arg(quantity));
    } else {
        status_->setText(tr("%1 ist in keiner Analyse gespeichert – unter \"outputs\" eintragen").arg(quantity));
    }
}

// --- subcircuits ---------------------------------------------------------------

void MainWindow::enterScope(const QString& scope) {
    if (!scope.isEmpty() && schematic_->schematic().spiceSubcircuits.contains(scope)) {
        // written in SPICE: no drawing, its text is the definition
        JValue root;
        const QString text = editor_->toPlainText();
        const JValue* subs = parseJsonc(text, root) ? root.get("subcircuits") : nullptr;
        const JValue* def = subs ? subs->get(scope) : nullptr;
        if (def) {
            showText_->setChecked(true);
            QTextCursor cur(editor_->document());
            cur.setPosition(def->begin);
            editor_->setTextCursor(cur);
            editor_->centerCursor();
            editor_->setFocus();
        }
        status_->setText(tr("„%1“ ist eine SPICE-Teilschaltung: ihr Text ist die Definition").arg(scope));
        return;
    }
    if (scope == scope_ && (scope.isEmpty() || scopeBar_->isVisible())) return;
    scope_ = scope;
    schematic_->selectComponents({});
    schematic_->resetView();
    palette_->setScope(scope);
    if (scope.isEmpty()) {
        scopeBar_->hide();
    } else {
        Schematic s;
        readSchematic(editor_->toPlainText(), s, nullptr, scope);
        scopeLabel_->setText(tr("Teilschaltung <b>%1</b> · Ports: %2").arg(scope.toHtmlEscaped(),
                                                                            s.ports.join(", ").toHtmlEscaped()));
        scopeBar_->show();
    }
    refreshSchematic();
    updateOperatingPoint();
    schematic_->setFocus();
}

void MainWindow::newSubcircuit() {
    const QString text = editor_->toPlainText();
    Schematic main;
    readSchematic(text, main);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Neue Teilschaltung"), tr("Name:"), QLineEdit::Normal,
                                               QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (main.subcircuits.contains(name)) {
        enterScope(name);
        return;
    }
    const QString list = QInputDialog::getText(this, tr("Neue Teilschaltung"),
                                               tr("Ports, durch Komma getrennt (z. B. in, out, vdd):"),
                                               QLineEdit::Normal, QStringLiteral("in, out"), &ok);
    if (!ok) return;
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    QStringList ports = list.split(separators, Qt::SkipEmptyParts);
    ports.removeDuplicates();
    ports.removeIf([](const QString& p) { return isGroundName(p); });  // ground is global, never a port
    const QString changed = addSubcircuit(text, name, ports);
    if (changed == text) {
        status_->setText(tr("Die Teilschaltung lässt sich nicht anlegen: \"subcircuits\" ist kein Objekt"));
        return;
    }
    applyText(changed);
    enterScope(name);
}

QString MainWindow::instanceQuantity(const QString& quantity, const Instance& in) const {
    static const QRegularExpression probe(QStringLiteral("^([vi])\\((.*)\\)$"));
    const auto m = probe.match(quantity);
    if (!m.hasMatch()) return quantity;
    const QString what = m.captured(1), name = m.captured(2);
    if (what == "v") {
        const int port = int(schematic_->schematic().ports.indexOf(name));
        if (port >= 0) return QStringLiteral("v(%1)").arg(in.nodes.value(port));  // the node outside
        if (isGroundName(name)) return quantity;
    }
    return QStringLiteral("%1(%2.%3)").arg(what, in.name, name);
}

void MainWindow::updateOperatingPoint() {
    if (scope_.isEmpty()) {
        schematic_->setOperatingPoint(op_);
        return;
    }
    // in a subcircuit: the values of its only instance
    QHash<QString, double> local;
    const QVector<Instance> instances = instancesOf(editor_->toPlainText(), scope_);
    if (instances.size() == 1 && !op_.isEmpty()) {
        const Schematic& s = schematic_->schematic();
        QSet<QString> names(s.ports.begin(), s.ports.end());
        for (const SLabel& l : s.labels) names.insert(l.name);
        for (const QString& n : names) {
            const QString v = QStringLiteral("v(%1)").arg(n);
            const QString global = instanceQuantity(v, instances.first());
            if (op_.contains(global)) local.insert(v, op_.value(global));
        }
        for (const SComponent& c : s.components) {
            const QString i = QStringLiteral("i(%1)").arg(c.name);
            const QString global = instanceQuantity(i, instances.first());
            if (op_.contains(global)) local.insert(i, op_.value(global));
        }
    }
    schematic_->setOperatingPoint(local);
}

// --- models ----------------------------------------------------------------------

QString MainWindow::importModels(QString text, const QStringList& names) const {
    for (const QString& name : names) {
        const LibraryModel* m = library_.find(name);
        if (!m || m->name != name) continue;
        const QStringList defined = m->type == "subckt" ? fileSubcircuits(text) : fileModels(text);
        if (!defined.contains(name)) text = addModel(text, *m);
    }
    return text;
}

void MainWindow::placeLibraryModel(const LibraryModel& m) {
    showPlan_->setChecked(true);
    if (m.type == "subckt") {
        SComponent c;
        c.type = QStringLiteral("X");
        c.kind = 'x';
        c.subcircuit = m.name;
        c.ports = m.ports;
        c.name = schematic_->schematic().freeName(QStringLiteral("X"));
        c.summary = m.name;
        c.nodes = QStringList(QVector<QString>(m.ports.size()));
        schematic_->startPlacing(c);
        return;
    }
    const QString id = m.type;  // the templates "d", "npn", "pmos", ... carry these ids
    for (const PartTemplate& t : partTemplates()) {
        if (t.id != id) continue;
        SComponent c = makeComponent(t, schematic_->schematic());
        c.model = m.name;
        c.summary = m.name;
        c.extra = QStringLiteral("\"model\": %1").arg(jsonQuote(m.name));
        if (m.text.contains(QRegularExpression(QStringLiteral(R"(level\s*=\s*(8|49|53)\b)"),
                                               QRegularExpression::CaseInsensitiveOption)))
            c.nodes = QStringList(QVector<QString>(4));  // BSIM3: with bulk
        showPlan_->setChecked(true);
        schematic_->startPlacing(c);
        return;
    }
}

void MainWindow::openLibrary() {
    ModelBrowser browser(&library_, examplesDir(), 0, false, this);
    const bool done = browser.exec() == QDialog::Accepted && browser.chosen();
    palette_->setLibrary(&library_.models());  // libraries may have changed
    if (!done) return;
    const LibraryModel m = *browser.chosen();
    if (browser.action() == ModelBrowser::Place) {
        placeLibraryModel(m);
    } else if (browser.action() == ModelBrowser::AddToFile) {
        const QString text = editor_->toPlainText();
        const QString changed = importModels(text, {m.name});
        status_->setText(changed == text ? tr("„%1“ ist schon in der Datei").arg(m.name)
                                         : tr("„%1“ in die Datei übernommen").arg(m.name));
        applyText(changed);
        refreshSchematic();
    }
}
