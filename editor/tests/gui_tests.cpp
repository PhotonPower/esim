// Tests of the schematic in the real main window: mouse and keyboard events
// on the drawing, and what arrives in the text. Runs without a screen
// (QT_QPA_PLATFORM=offscreen); the settings go to a temporary directory.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QTreeWidget>
#include <QTimer>
#include <QComboBox>
#include <QProcess>
#include <QStackedWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <cstdio>
#include <cstdlib>

#include "Checker.hpp"
#include "CodeEditor.hpp"
#include "MainWindow.hpp"
#include "ModelBrowser.hpp"
#include "HistogramWidget.hpp"
#include "PlotWidget.hpp"
#include "ResultsPanel.hpp"
#include "PartPalette.hpp"
#include "PropertyPanel.hpp"
#include "Schematic.hpp"
#include "SchematicView.hpp"

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const QString& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s\n", qPrintable(what));
    }
}

SchematicView* view;
CodeEditor* editor;

QPoint at(int x, int y) { return view->gridToWidget(QPointF(x, y)).toPoint(); }

void click(int x, int y, Qt::KeyboardModifiers mods = {}) {
    QTest::mouseMove(view, at(x, y));
    QTest::mouseClick(view, Qt::LeftButton, mods, at(x, y));
}

void key(Qt::Key k, Qt::KeyboardModifiers mods = {}) { QTest::keyClick(view, k, mods); }

const SComponent* comp(const QString& name) {
    const int k = view->schematic().find(name);
    return k < 0 ? nullptr : &view->schematic().components[k];
}

QString nodesOf(const QString& name) {
    const SComponent* c = comp(name);
    return c ? c->nodes.join(",") : QStringLiteral("(missing)");
}

PartTemplate part(const char* id) {
    for (const PartTemplate& p : partTemplates())
        if (p.id == id) return p;
    return partTemplates().first();
}

bool errorFree(const QString& text) {
    const CheckResult r = checkText(text, QDir::tempPath());
    for (const Diagnostic& d : r.diagnostics)
        if (d.severity == Diagnostic::Error) return false;
    return r.ok;
}

}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("esim-tests");
    QCoreApplication::setApplicationName("gui-tests");

    QTemporaryDir dir;
    const QString path = dir.filePath("t.json");
    {
        QFile f(path);
        check(f.open(QIODevice::WriteOnly), "test file");
        f.write("{\n  // a test\n  \"components\": [],\n  \"analyses\": [{ \"type\": \"op\" }]\n}\n");
    }
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    check(w.openFile(path), "file opened");
    view = w.findChild<SchematicView*>();
    editor = w.findChild<CodeEditor*>();
    check(view && editor, "widgets found");
    if (!view || !editor) return 1;
    QApplication::processEvents();
    view->repaint();  // fits the (empty) view

    std::printf("[test] place parts, ground and wire\n");
    view->startPlacing(makeComponent(part("r"), view->schematic()));
    click(0, 0);
    key(Qt::Key_Escape);
    check(comp("R1") && comp("R1")->place.at == QPoint(0, 0), "R1 placed at (0, 0)");
    check(editor->toPlainText().contains("\"name\": \"R1\"") && editor->toPlainText().contains("// a test"),
          "R1 in the text, comment kept:\n" + editor->toPlainText());

    view->startGround();
    click(0, 2);
    key(Qt::Key_Escape);
    check(nodesOf("R1").endsWith(",0"), "R1 grounded: " + nodesOf("R1"));

    view->startWire();
    click(0, -2);
    click(4, -2);
    key(Qt::Key_Return);
    key(Qt::Key_Escape);
    check(view->schematic().wires.size() == 1, "one wire");

    view->startPlacing(makeComponent(part("v"), view->schematic()));
    click(4, 0);  // the + pin meets the end of the wire
    key(Qt::Key_Escape);
    view->startGround();
    click(4, 2);
    key(Qt::Key_Escape);
    check(comp("V1") && nodesOf("V1").split(",").value(0) == nodesOf("R1").split(",").value(0) &&
              nodesOf("V1").endsWith(",0"),
          "V1 and R1 connected: " + nodesOf("V1") + " / " + nodesOf("R1"));
    check(errorFree(editor->toPlainText()), "drawn circuit passes the check:\n" + editor->toPlainText());

    std::printf("[test] move, rotate, undo, delete\n");
    QTest::mousePress(view, Qt::LeftButton, {}, at(0, 0));
    QTest::mouseMove(view, at(-1, 0));
    QTest::mouseMove(view, at(-2, 0));
    QTest::mouseRelease(view, Qt::LeftButton, {}, at(-2, 0));
    check(comp("R1") && comp("R1")->place.at == QPoint(-2, 0), "R1 dragged to (-2, 0)");
    check(view->schematic().wires.value(0).pts.value(0) == QPoint(-2, -2), "wire end follows the pin");
    check(view->schematic().grounds.value(0).at == QPoint(-2, 2), "ground at the pin follows");
    const QString before = nodesOf("R1");
    check(view->connectivity().conflicts.isEmpty() && nodesOf("V1").startsWith(before.split(",").value(0)),
          "still connected after the move");

    key(Qt::Key_R);
    check(comp("R1") && comp("R1")->place.rot == 90, "R1 rotated");
    check(nodesOf("R1") == before, "rotation keeps the connections: " + nodesOf("R1"));

    editor->undo();
    QTest::qWait(300);  // the schematic reads the text again
    check(comp("R1") && comp("R1")->place.rot == 0 && comp("R1")->place.at == QPoint(-2, 0),
          "undo in the text undoes the rotation");
    editor->undo();
    QTest::qWait(300);
    check(comp("R1") && comp("R1")->place.at == QPoint(0, 0), "second undo undoes the move");
    editor->redo();
    QTest::qWait(300);
    check(comp("R1") && comp("R1")->place.at == QPoint(-2, 0), "redo");

    click(-2, 0);
    check(view->selectedComponents() == QStringList{"R1"}, "R1 selected by a click");
    key(Qt::Key_D, Qt::ControlModifier);
    check(comp("R2") != nullptr && editor->toPlainText().contains("\"name\": \"R2\""), "⌘D duplicates R1 as R2");
    key(Qt::Key_Delete);
    check(!comp("R2"), "the copy deleted");
    click(-2, 0);
    key(Qt::Key_Delete);
    check(!comp("R1") && !editor->toPlainText().contains("\"R1\""), "R1 deleted from drawing and text");
    check(comp("V1") != nullptr, "V1 stays");

    std::printf("[test] properties and text\n");
    click(4, 0);
    check(view->selectedComponents() == QStringList{"V1"}, "V1 selected");
    auto* props = w.findChild<PropertyPanel*>();
    QLineEdit* dc = nullptr;
    for (QLineEdit* e : props->findChildren<QLineEdit*>())
        if (e->text() == "1") dc = e;
    check(dc != nullptr, "field of dc shown");
    if (dc) {
        dc->setText("2.5");
        emit dc->editingFinished();
        check(editor->toPlainText().contains("\"dc\": 2.5"), "dc changed in the text:\n" + editor->toPlainText());
    }
    QLineEdit* name = nullptr;
    for (QLineEdit* e : props->findChildren<QLineEdit*>())
        if (e->text() == "V1") name = e;
    if (name) {
        name->setText("Vsupply");
        emit name->editingFinished();
    }
    check(comp("Vsupply") && editor->toPlainText().contains("\"Vsupply\": { \"at\""), "renamed in text and drawing");

    // the text edited by hand: the drawing follows
    QString text = editor->toPlainText();
    text.replace("\"dc\": 2.5", "\"dc\": 3");
    editor->selectAll();
    editor->insertPlainText(text);
    QTest::qWait(300);
    check(comp("Vsupply") && comp("Vsupply")->summary == "3", "drawing follows the text: " +
                                                                    (comp("Vsupply") ? comp("Vsupply")->summary : QString()));
    editor->selectAll();
    editor->insertPlainText("{ \"components\": [");
    QTest::qWait(300);
    check(comp("Vsupply") != nullptr, "invalid text: the drawing keeps its last state");

    std::printf("[test] subcircuits\n");
    check(w.openFile(QStringLiteral(ESIM_SOURCE_DIR "/examples/ring_oscillator_bsim3.json")), "ring oscillator opened");
    QApplication::processEvents();
    view->repaint();
    const SComponent* u1 = comp("U1");
    check(u1 != nullptr, "instance U1 shown");
    if (u1) {
        const QPoint p = at(u1->place.at.x(), u1->place.at.y());
        QTest::mouseDClick(view, Qt::LeftButton, {}, p);
        check(view->schematic().scope == "inv" && comp("MP") && comp("MN"), "double click opens the subcircuit");
        key(Qt::Key_Up, Qt::ControlModifier);
        check(view->schematic().scope.isEmpty() && comp("U1"), "⌘↑ goes back");
    }
    // the text cursor in a component of the subcircuit takes the drawing there
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(int(editor->toPlainText().indexOf("\"MN\"")));
    editor->setTextCursor(cursor);
    QApplication::processEvents();
    check(view->schematic().scope == "inv" && view->selectedComponents() == QStringList{"MN"},
          "cursor in MN shows inv with MN selected");
    cursor.setPosition(int(editor->toPlainText().indexOf("\"U3\"")));
    editor->setTextCursor(cursor);
    QApplication::processEvents();
    check(view->schematic().scope.isEmpty() && view->selectedComponents() == QStringList{"U3"},
          "cursor in U3 goes back with U3 selected");

    std::printf("[test] selection into a subcircuit\n");
    {
        // V1 and R1 to ground through R2, drawn; R1 and R2 go into a subcircuit
        QFile f(path);
        check(f.open(QIODevice::WriteOnly), "test file for the extraction");
        f.write("{\n  \"components\": [],\n  \"analyses\": [{ \"type\": \"op\" }]\n}\n");
    }
    check(w.openFile(path), "file opened");
    QApplication::processEvents();
    view->repaint();
    view->startPlacing(makeComponent(part("v"), view->schematic()));
    click(0, 0);
    key(Qt::Key_Escape);
    view->startPlacing(makeComponent(part("r"), view->schematic()));
    click(6, 0);
    click(10, 0);
    key(Qt::Key_Escape);
    view->startGround();
    click(0, 2);
    click(10, 2);
    key(Qt::Key_Escape);
    view->startWire();
    click(0, -2);
    click(6, -2);  // ends at the pin of R1
    key(Qt::Key_Escape);
    view->startWire();
    click(6, 2);
    click(10, -2);  // R1 bottom to R2 top
    key(Qt::Key_Escape);
    const QString drawn = editor->toPlainText();
    // frame around R1 and R2
    QTest::mousePress(view, Qt::LeftButton, {}, at(4, -4));
    QTest::mouseMove(view, at(8, 0));
    QTest::mouseMove(view, at(12, 4));
    QTest::mouseRelease(view, Qt::LeftButton, {}, at(12, 4));
    check(view->selection().comps == QSet<QString>({"R1", "R2"}), "R1 and R2 selected by a frame");
    // the dialog for the name: accept the proposal
    QTimer::singleShot(200, []() {
        if (QWidget* d = QApplication::activeModalWidget()) QMetaObject::invokeMethod(d, "accept");
    });
    key(Qt::Key_G, Qt::ControlModifier);
    QApplication::processEvents();
    const QString done = editor->toPlainText();
    check(done.contains("\"teil1\": {") && done.contains("\"subcircuit\": \"teil1\"") && !comp("R1"),
          "⌘G made teil1:\n" + done);
    check(errorFree(done), "the result passes the check");
    {
        // the connections stay: V1 still drives R1 and R2 (nodes of the drawing)
        Schematic top;
        readSchematic(done, top);
        const Connectivity cn = connectSchematic(top);
        const int v1 = top.find("V1"), x1 = top.find("X1");
        check(v1 >= 0 && x1 >= 0 && cn.pinNodes[v1][0] == cn.pinNodes[x1][0],
              "V1 still connected to the subcircuit: " + (v1 >= 0 ? cn.pinNodes[v1].join(",") : QString()) + " / " +
                  (x1 >= 0 ? cn.pinNodes[x1].join(",") : QString()));
    }
    if (!qEnvironmentVariableIsEmpty("ESIM_SNAPSHOT_DIR")) {
        const QString dir = qEnvironmentVariable("ESIM_SNAPSHOT_DIR");
        view->fitAll();
        view->grab().save(dir + "/extract_top.png");
        w.showSubcircuit("teil1");
        QApplication::processEvents();
        view->fitAll();
        view->grab().save(dir + "/extract_sub.png");
        w.showSubcircuit(QString());
        QApplication::processEvents();
    }
    editor->undo();
    QTest::qWait(300);
    check(editor->toPlainText() == drawn && comp("R1") && comp("R2"), "one undo brings everything back");

    std::printf("[test] model library\n");
    {
        QFile f(path);
        check(f.open(QIODevice::WriteOnly), "test file again");
        f.write("{\n  \"components\": [],\n  \"analyses\": [{ \"type\": \"op\" }]\n}\n");
    }
    check(w.openFile(path), "empty file opened");
    QApplication::processEvents();
    view->repaint();
    auto* palette = w.findChild<PartPalette*>();
    // the palette offers library models while searching: find the id of NPN_GEN
    QString id;
    for (QListWidget* list : palette->findChildren<QListWidget*>())
        for (int k = 0; k < list->count(); ++k)
            if (list->item(k)->text().startsWith("NPN_GEN")) id = list->item(k)->data(Qt::UserRole).toString();
    check(id.startsWith("lib:"), "NPN_GEN in the part list: " + id);
    emit palette->partChosen(id);
    click(0, 0);
    key(Qt::Key_Escape);
    const QString t = editor->toPlainText();
    check(comp("Q1") && comp("Q1")->model == "NPN_GEN", "Q1 placed with the model");
    check(t.contains("\"NPN_GEN\": \".model NPN_GEN") && t.contains("\"model\": \"NPN_GEN\""),
          "card copied into the file:\n" + t);
    // a second one does not copy the card again
    emit palette->partChosen(id);
    click(6, 0);
    key(Qt::Key_Escape);
    check(editor->toPlainText().count("\"NPN_GEN\": ") == 1 && comp("Q2"), "card copied once");

    // the model field of the properties takes a library name as well
    click(0, 0);
    auto* props2 = w.findChild<PropertyPanel*>();
    QLineEdit* modelField = nullptr;
    for (QLineEdit* e : props2->findChildren<QLineEdit*>())
        if (e->text() == "NPN_GEN") modelField = e;
    check(modelField != nullptr, "model field shown");
    if (modelField) {
        modelField->setText("BC547");  // from the examples
        emit modelField->editingFinished();
        check(editor->toPlainText().contains("\"BC547\": \".model BC547B") && comp("Q1") && comp("Q1")->model == "BC547",
              "model changed, card of BC547 copied from the example");
    }

    {
        ModelLibrary library;
        library.load({}, QStringLiteral(ESIM_SOURCE_DIR "/examples"));
        ModelBrowser browser(&library, QString(), 'q', false);
        browser.show();
        auto* tree = browser.findChild<QTreeWidget*>();
        auto* search = browser.findChild<QLineEdit*>();
        check(tree && tree->topLevelItemCount() >= 4, "browser lists the bipolar transistors");
        if (search && tree) {
            search->setText("lehrbuch");
            check(tree->topLevelItemCount() == 2, QString("search in the descriptions: %1").arg(tree->topLevelItemCount()));
            search->clear();
        }
        if (!qEnvironmentVariableIsEmpty("ESIM_SNAPSHOT_DIR")) {
            tree->setCurrentItem(tree->topLevelItem(0));
            QApplication::processEvents();
            browser.grab().save(qEnvironmentVariable("ESIM_SNAPSHOT_DIR") + "/library.png");
        }
    }

    std::printf("[test] measurements: histogram, points, curve families\n");
    {
        QTemporaryDir out;
        QProcess esim;
        esim.start(ESIM_BINARY, {ESIM_SOURCE_DIR "/examples/monte_carlo_filter.json", "-o", out.path(), "--summary",
                                 out.filePath("s.json")});
        check(esim.waitForFinished(60000) && esim.exitCode() == 0, "esim ran the Monte Carlo example");
        ResultsPanel panel;
        panel.resize(1100, 640);
        panel.show();
        QString error;
        check(panel.load(out.filePath("s.json"), error), "summary loaded: " + error);
        auto* stack = panel.findChild<QStackedWidget*>();
        auto* hist = panel.findChild<HistogramWidget*>();
        auto* view = panel.findChild<QComboBox*>();
        check(stack && hist && stack->currentWidget() == hist, "Monte Carlo shows histograms");
        check(view && view->count() == 3 && view->itemText(1).contains("C"), "views: histogram, over R, over C");
        const QString dir = qEnvironmentVariable("ESIM_SNAPSHOT_DIR");
        QApplication::processEvents();
        if (!dir.isEmpty()) panel.grab().save(dir + "/mc_hist.png");
        if (view) view->setCurrentIndex(view->findData("C"));
        check(stack && stack->currentWidget() == panel.plot(), "points over C");
        if (!dir.isEmpty()) panel.grab().save(dir + "/mc_points.png");
        // the nested sweep: fc over C, one curve per R
        auto* analyses = panel.findChild<QListWidget*>("analyses");
        if (analyses) analyses->setCurrentRow(1);
        check(view && view->count() == 2 && view->currentData().toString() == "R", QString("sweep over R by default: %1 views, %2").arg(view ? view->count() : -1).arg(view ? view->currentData().toString() + " / " + view->currentText() : QString()));
        if (view) view->setCurrentIndex(view->findData("C"));
        QApplication::processEvents();
        if (!dir.isEmpty()) panel.grab().save(dir + "/grid.png");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    // no question about unsaved changes
    QFile::remove(path);
    std::fflush(stdout);
    std::_Exit(g_failures ? 1 : 0);
}
