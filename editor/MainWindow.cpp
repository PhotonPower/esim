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
#include <QStatusBar>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>

#include "CodeEditor.hpp"
#include "PlotWidget.hpp"
#include "ResultsPanel.hpp"
#include "SimulationRun.hpp"
#include "Theme.hpp"

namespace {

constexpr int kMaxRecent = 10;

const char* kTemplate = R"json({
  "title": "Neue Schaltung",

  "components": [
    { "type": "V", "name": "V1", "nodes": ["in", "0"],
      "wave": { "type": "pulse", "v1": 0, "v2": 1, "td": "1m", "tr": "1u", "tf": "1u", "pw": "5m", "per": "10m" } },
    { "type": "R", "name": "R1", "nodes": ["in", "out"], "value": "1k" },
    { "type": "C", "name": "C1", "nodes": ["out", "0"], "value": "1u" }
  ],

  "analyses": [
    { "type": "op" },
    { "type": "tran", "tstop": "20m" }
  ],

  "outputs": ["v(in)", "v(out)"]
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
    setCentralWidget(editor_);
    run_ = new SimulationRun(this);
    checkTimer_ = new QTimer(this);
    checkTimer_->setSingleShot(true);
    checkTimer_->setInterval(400);

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
    });
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
    if (!restoreState(settings.value("window/state").toByteArray())) resize(1280, 860);
    updateRecentMenu();
    newFile();
}

// --- menus and docks --------------------------------------------------------

void MainWindow::createDocks() {
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
    resizeDocks({resultsDock_}, {420}, Qt::Vertical);
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

    QMenu* sim = menuBar()->addMenu(tr("&Simulation"));
    runAction_ = sim->addAction(tr("&Starten"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::runSimulation);
    stopAction_ = sim->addAction(tr("Sto&ppen"), QKeySequence(Qt::CTRL | Qt::Key_Period), this, &MainWindow::stopSimulation);
    stopAction_->setEnabled(false);

    QMenu* view = menuBar()->addMenu(tr("&Ansicht"));
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
    results_->clear();
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
    results_->clear();
    log_->clear();
    scheduleCheck();
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
    editor_->setDiagnostics(r.diagnostics);
    problems_->clear();
    int errors = 0, warnings = 0;
    for (const Diagnostic& d : r.diagnostics) {
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
    resultsDock_->show();
    resultsDock_->raise();
}
