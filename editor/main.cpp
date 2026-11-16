#include <QApplication>
#include <QFileOpenEvent>

#include "MainWindow.hpp"

namespace {

// Files opened from the Finder (double click, drop on the dock icon) arrive
// as events of the application.
class Application : public QApplication {
public:
    using QApplication::QApplication;
    MainWindow* window = nullptr;

protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::FileOpen && window) {
            window->openFile(static_cast<QFileOpenEvent*>(e)->file());
            return true;
        }
        return QApplication::event(e);
    }
};

}  // namespace

int main(int argc, char** argv) {
    Application app(argc, argv);
    QApplication::setOrganizationName("esim");
    QApplication::setApplicationName("ESIM Editor");
    QApplication::setApplicationVersion("0.1.0");

    MainWindow window;
    app.window = &window;
    // esim-editor [file.json] [--subcircuit name] [--run] [--snapshot image.png]
    QStringList args = QApplication::arguments();
    args.removeFirst();
    bool run = false;
    QString snapshot, file, subcircuit;
    for (int k = 0; k < args.size(); ++k) {
        if (args[k] == "--run") run = true;
        else if (args[k] == "--snapshot" && k + 1 < args.size()) snapshot = args[++k];
        else if (args[k] == "--subcircuit" && k + 1 < args.size()) subcircuit = args[++k];
        else if (!args[k].startsWith('-')) file = args[k];
    }
    if (!file.isEmpty() && window.openFile(file) && !subcircuit.isEmpty()) window.showSubcircuit(subcircuit);
    window.show();
    if (run || !snapshot.isEmpty()) window.automate(run && !file.isEmpty(), snapshot);
    return app.exec();
}
