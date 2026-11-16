#include "JsonHighlighter.hpp"

#include "Theme.hpp"

JsonHighlighter::JsonHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) { updateColors(); }

void JsonHighlighter::updateColors() {
    const bool dark = Theme::dark();
    key_.setForeground(QColor(dark ? "#7cb4f0" : "#1c5cab"));
    string_.setForeground(QColor(dark ? "#5fc79b" : "#0f7a50"));
    number_.setForeground(QColor(dark ? "#e8a35c" : "#b5530f"));
    keyword_.setForeground(QColor(dark ? "#b3a9f2" : "#5040b8"));
    comment_.setForeground(Theme::textSecondary());
    comment_.setFontItalic(true);
    punct_.setForeground(dark ? QColor("#a8a79f") : QColor("#6b6a64"));
    rehighlight();
}

// Block state 1: the line ends inside a /* */ comment.
void JsonHighlighter::highlightBlock(const QString& text) {
    const int n = text.size();
    int i = 0;
    if (previousBlockState() == 1) {
        const int end = text.indexOf("*/");
        if (end < 0) {
            setFormat(0, n, comment_);
            setCurrentBlockState(1);
            return;
        }
        setFormat(0, end + 2, comment_);
        i = end + 2;
    }
    setCurrentBlockState(0);
    while (i < n) {
        const QChar c = text[i];
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            setFormat(i, n - i, comment_);
            return;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            const int end = text.indexOf("*/", i + 2);
            if (end < 0) {
                setFormat(i, n - i, comment_);
                setCurrentBlockState(1);
                return;
            }
            setFormat(i, end + 2 - i, comment_);
            i = end + 2;
            continue;
        }
        if (c == '"') {
            int j = i + 1;
            while (j < n && text[j] != '"') j += text[j] == '\\' ? 2 : 1;
            const int len = std::min(j + 1, n) - i;
            // a key is a string followed by a colon
            int k = j + 1;
            while (k < n && text[k].isSpace()) ++k;
            setFormat(i, len, k < n && text[k] == ':' ? key_ : string_);
            i += len;
            continue;
        }
        if (c.isDigit() || ((c == '-' || c == '+' || c == '.') && i + 1 < n && (text[i + 1].isDigit() || text[i + 1] == '.'))) {
            int j = i + 1;
            while (j < n && (text[j].isLetterOrNumber() || text[j] == '.' || text[j] == '+' || text[j] == '-')) ++j;
            setFormat(i, j - i, number_);
            i = j;
            continue;
        }
        if (c.isLetter()) {
            int j = i;
            while (j < n && text[j].isLetter()) ++j;
            const QString word = text.mid(i, j - i);
            if (word == "true" || word == "false" || word == "null") setFormat(i, j - i, keyword_);
            i = j;
            continue;
        }
        if (QStringLiteral("{}[]:,").contains(c)) setFormat(i, 1, punct_);
        ++i;
    }
}
