#pragma once

#include <QPlainTextEdit>
#include <QVector>

class JsonHighlighter;

// A finding of the check of the input, at a line of the text (0-based; -1:
// not tied to a line).
struct Diagnostic {
    enum Severity { Error, Warning };
    Severity severity = Error;
    int line = -1;
    int column = -1;  // 0-based, -1: the whole line
    QString message;
};

// Text editor for the input: line numbers, colours, marks for errors and
// warnings, indentation that follows the brackets, comment toggling (⌘/).
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget* parent = nullptr);

    void setDiagnostics(const QVector<Diagnostic>& diagnostics);
    void goToLine(int line, int column = -1);
    void toggleComment();
    void updateColors();
    // Marks the text range [begin, end) (the component selected in the
    // schematic); -1: none.
    void setHighlight(int begin, int end);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    friend class LineNumberArea;
    int gutterWidth() const;
    void paintGutter(QPaintEvent* e);
    void updateGutter(const QRect& rect, int dy);
    void updateSelections();
    void indentSelection(bool out);

    QWidget* gutter_;
    JsonHighlighter* highlighter_;
    QVector<Diagnostic> diagnostics_;
    int highlightBegin_ = -1, highlightEnd_ = -1;
};
