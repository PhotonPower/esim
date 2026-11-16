// Tests of the schematic in the real main window: mouse and keyboard events
// on the drawing, and what arrives in the text. Runs without a screen
// (QT_QPA_PLATFORM=offscreen); the settings go to a temporary directory.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <cstdio>
#include <unistd.h>

#include "Checker.hpp"
#include "CodeEditor.hpp"
#include "MainWindow.hpp"
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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    // no question about unsaved changes
    QFile::remove(path);
    std::fflush(stdout);
    _exit(g_failures ? 1 : 0);
}
