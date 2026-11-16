#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

// Colours for the input format: JSON with // and /* */ comments. Keys,
// strings, numbers, true/false/null and comments get their own format.
class JsonHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit JsonHighlighter(QTextDocument* doc);
    void updateColors();  // after a change of the appearance (light/dark)

protected:
    void highlightBlock(const QString& text) override;

private:
    QTextCharFormat key_, string_, number_, keyword_, comment_, punct_;
};
