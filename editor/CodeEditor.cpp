#include "CodeEditor.hpp"

#include <QFontDatabase>
#include <QHelpEvent>
#include <QPainter>
#include <QTextBlock>
#include <QToolTip>

#include "JsonHighlighter.hpp"
#include "Theme.hpp"

namespace {
constexpr int kIndent = 2;
QColor errorColor() { return Theme::dark() ? QColor("#e66767") : QColor("#d03b3b"); }
QColor warningColor() { return Theme::dark() ? QColor("#c98500") : QColor("#b37400"); }
}  // namespace

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor* editor) : QWidget(editor), editor_(editor) {}
    QSize sizeHint() const override { return {editor_->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* e) override { editor_->paintGutter(e); }

private:
    CodeEditor* editor_;
};

CodeEditor::CodeEditor(QWidget* parent) : QPlainTextEdit(parent) {
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(std::max(mono.pointSizeF(), 12.0));
    setFont(mono);
    setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(' ') * kIndent);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    gutter_ = new LineNumberArea(this);
    highlighter_ = new JsonHighlighter(document());
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this]() { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateGutter);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::updateSelections);
    setViewportMargins(gutterWidth(), 0, 0, 0);
    updateSelections();
}

int CodeEditor::gutterWidth() const {
    int digits = 1;
    for (int n = std::max(1, blockCount()); n >= 10; n /= 10) ++digits;
    return 22 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(digits, 3);
}

void CodeEditor::updateGutter(const QRect& rect, int dy) {
    if (dy) gutter_->scroll(0, dy);
    else gutter_->update(0, rect.y(), gutter_->width(), rect.height());
}

void CodeEditor::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    const QRect cr = contentsRect();
    gutter_->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
}

void CodeEditor::paintGutter(QPaintEvent* e) {
    QPainter p(gutter_);
    p.fillRect(e->rect(), palette().color(QPalette::Base));
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();
    while (block.isValid() && top <= e->rect().bottom()) {
        if (block.isVisible() && bottom >= e->rect().top()) {
            // mark: the most severe diagnostic of the line
            int severity = -1;
            for (const Diagnostic& d : diagnostics_)
                if (d.line == number) severity = std::max(severity, d.severity == Diagnostic::Error ? 1 : 0);
            if (severity >= 0) {
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(Qt::NoPen);
                p.setBrush(severity == 1 ? errorColor() : warningColor());
                const int h = fontMetrics().height();
                p.drawEllipse(QPointF(8, top + h / 2.0), 4, 4);
            }
            p.setPen(number == current ? Theme::textPrimary() : Theme::textSecondary());
            p.drawText(0, top, gutter_->width() - 6, fontMetrics().height(), Qt::AlignRight,
                       QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void CodeEditor::setDiagnostics(const QVector<Diagnostic>& diagnostics) {
    diagnostics_ = diagnostics;
    updateSelections();
    gutter_->update();
}

void CodeEditor::updateSelections() {
    QList<QTextEdit::ExtraSelection> list;
    QTextEdit::ExtraSelection line;
    QColor band = Theme::dark() ? QColor(255, 255, 255, 14) : QColor(0, 0, 0, 10);
    line.format.setBackground(band);
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    list << line;
    for (const Diagnostic& d : diagnostics_) {
        if (d.line < 0) continue;
        const QTextBlock block = document()->findBlockByNumber(d.line);
        if (!block.isValid()) continue;
        QTextEdit::ExtraSelection s;
        s.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        s.format.setUnderlineColor(d.severity == Diagnostic::Error ? errorColor() : warningColor());
        s.cursor = QTextCursor(block);
        const QString text = block.text();
        int from = 0;  // from the first character that is not indentation
        while (from < text.size() && text[from].isSpace()) ++from;
        if (d.column >= 0 && d.column < text.size()) from = d.column;
        s.cursor.setPosition(block.position() + from);
        s.cursor.setPosition(block.position() + std::max<int>(text.size(), from + 1), QTextCursor::KeepAnchor);
        list << s;
    }
    setExtraSelections(list);
}

void CodeEditor::goToLine(int line, int column) {
    const QTextBlock block = document()->findBlockByNumber(std::max(line, 0));
    if (!block.isValid()) return;
    QTextCursor c(block);
    if (column > 0) c.setPosition(block.position() + std::min(column, block.length() - 1));
    setTextCursor(c);
    centerCursor();
    setFocus();
}

bool CodeEditor::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(e);
        const QPoint pos = help->pos() - QPoint(viewportMargins().left(), 0);
        const int line = cursorForPosition(pos).blockNumber();
        QStringList messages;
        for (const Diagnostic& d : diagnostics_)
            if (d.line == line) messages << d.message;
        if (!messages.isEmpty()) QToolTip::showText(help->globalPos(), messages.join('\n'), this);
        else QToolTip::hideText();
        return true;
    }
    return QPlainTextEdit::event(e);
}

void CodeEditor::changeEvent(QEvent* e) {
    QPlainTextEdit::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ThemeChange) updateColors();
}

void CodeEditor::updateColors() {
    highlighter_->updateColors();
    updateSelections();
    gutter_->update();
}

void CodeEditor::keyPressEvent(QKeyEvent* e) {
    QTextCursor c = textCursor();
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        // keep the indentation; one step more after an opening bracket, and
        // a closing bracket right after the cursor moves to its own line
        const QString text = c.block().text();
        QString indent;
        for (QChar ch : text) {
            if (ch != ' ' && ch != '\t') break;
            indent += ch;
        }
        const QString before = text.left(c.positionInBlock()).trimmed();
        const QString after = text.mid(c.positionInBlock()).trimmed();
        const bool opens = before.endsWith('{') || before.endsWith('[');
        const bool closes = after.startsWith('}') || after.startsWith(']');
        c.beginEditBlock();
        c.insertText("\n" + indent + (opens ? QString(kIndent, ' ') : QString()));
        if (opens && closes) {
            const int pos = c.position();
            c.insertText("\n" + indent);
            c.setPosition(pos);
        }
        c.endEditBlock();
        setTextCursor(c);
        return;
    }
    if (e->key() == Qt::Key_Tab && !(e->modifiers() & Qt::ShiftModifier)) {
        if (c.hasSelection() && c.selectedText().contains(QChar::ParagraphSeparator)) indentSelection(false);
        else c.insertText(QString(kIndent - c.positionInBlock() % kIndent, ' '));
        return;
    }
    if (e->key() == Qt::Key_Backtab) {
        indentSelection(true);
        return;
    }
    if (e->key() == Qt::Key_Backspace && !c.hasSelection()) {
        // inside the indentation: remove a whole step
        const QString left = c.block().text().left(c.positionInBlock());
        if (!left.isEmpty() && left.trimmed().isEmpty()) {
            const int remove = (left.size() - 1) % kIndent + 1;
            c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, remove);
            c.removeSelectedText();
            return;
        }
    }
    if (e->key() == Qt::Key_BraceRight || e->key() == Qt::Key_BracketRight) {
        // a closing bracket on an otherwise empty line goes back one step
        const QString left = c.block().text().left(c.positionInBlock());
        if (left.size() >= kIndent && left.trimmed().isEmpty()) {
            c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, kIndent);
            c.removeSelectedText();
        }
    }
    QPlainTextEdit::keyPressEvent(e);
}

void CodeEditor::indentSelection(bool out) {
    QTextCursor c = textCursor();
    const int start = c.selectionStart(), end = c.selectionEnd();
    QTextBlock block = document()->findBlock(start);
    const QTextBlock last = document()->findBlock(end > start ? end - 1 : end);
    c.beginEditBlock();
    while (block.isValid()) {
        QTextCursor b(block);
        if (out) {
            int n = 0;
            while (n < kIndent && n < block.text().size() && block.text()[n] == ' ') ++n;
            b.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, n);
            b.removeSelectedText();
        } else {
            b.insertText(QString(kIndent, ' '));
        }
        if (block == last) break;
        block = block.next();
    }
    c.endEditBlock();
}

void CodeEditor::toggleComment() {
    QTextCursor c = textCursor();
    const int start = c.selectionStart(), end = c.selectionEnd();
    QTextBlock first = document()->findBlock(start);
    const QTextBlock last = document()->findBlock(end > start && c.hasSelection() ? end - 1 : end);
    // all lines commented: uncomment; otherwise comment all
    bool all = true;
    int column = INT_MAX;
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        const QString t = b.text();
        if (!t.trimmed().isEmpty()) {
            all = all && t.trimmed().startsWith("//");
            int k = 0;
            while (k < t.size() && t[k] == ' ') ++k;
            column = std::min(column, k);
        }
        if (b == last) break;
    }
    if (column == INT_MAX) column = 0;
    c.beginEditBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        const QString t = b.text();
        QTextCursor bc(b);
        if (all) {
            const int k = t.indexOf("//");
            if (k >= 0) {
                bc.setPosition(b.position() + k);
                bc.setPosition(b.position() + k + (t.mid(k, 3) == "// " ? 3 : 2), QTextCursor::KeepAnchor);
                bc.removeSelectedText();
            }
        } else if (!t.trimmed().isEmpty()) {
            bc.setPosition(b.position() + column);
            bc.insertText("// ");
        }
        if (b == last) break;
    }
    c.endEditBlock();
}
