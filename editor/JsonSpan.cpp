#include "JsonSpan.hpp"

namespace {

class Parser {
public:
    explicit Parser(const QString& text) : s_(text), n_(int(text.size())) {}

    bool parse(JValue& root) {
        skip();
        if (!value(root, 0)) return false;
        skip();
        if (failed_) return false;
        if (i_ < n_) return fail(QStringLiteral("unexpected text after the end"));
        return true;
    }

    QString error;
    int errorPos = 0;

private:
    bool fail(const QString& what) {
        if (!failed_) {
            failed_ = true;
            error = what;
            errorPos = i_;
        }
        return false;
    }

    // whitespace and comments
    void skip() {
        while (i_ < n_) {
            const QChar c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else if (c == '/' && i_ + 1 < n_ && s_[i_ + 1] == '/') {
                while (i_ < n_ && s_[i_] != '\n') ++i_;
            } else if (c == '/' && i_ + 1 < n_ && s_[i_ + 1] == '*') {
                const int close = int(s_.indexOf(QStringLiteral("*/"), i_ + 2));
                if (close < 0) {
                    fail(QStringLiteral("comment not closed"));
                    i_ = n_;
                    return;
                }
                i_ = close + 2;
            } else {
                return;
            }
        }
    }

    bool value(JValue& v, int depth) {
        if (depth > 200) return fail(QStringLiteral("nested too deeply"));
        if (i_ >= n_) return fail(QStringLiteral("unexpected end of the text"));
        v.begin = i_;
        const QChar c = s_[i_];
        bool ok;
        if (c == '{') ok = object(v, depth);
        else if (c == '[') ok = array(v, depth);
        else if (c == '"') {
            v.type = JValue::String;
            ok = string(v.string);
        } else if (c == '-' || c.isDigit()) ok = number(v);
        else ok = word(v);
        v.end = i_;
        return ok;
    }

    bool object(JValue& v, int depth) {
        v.type = JValue::Object;
        ++i_;
        skip();
        if (i_ < n_ && s_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            skip();
            if (i_ >= n_ || s_[i_] != '"') return fail(QStringLiteral("expected a key in quotes"));
            const int keyBegin = i_;
            QString key;
            if (!string(key)) return false;
            skip();
            if (i_ >= n_ || s_[i_] != ':') return fail(QStringLiteral("expected ':'"));
            ++i_;
            skip();
            JValue item;
            if (!value(item, depth + 1)) return false;
            v.keys << key;
            v.keyBegins.push_back(keyBegin);
            v.items.push_back(std::move(item));
            skip();
            if (i_ < n_ && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < n_ && s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail(QStringLiteral("expected ',' or '}'"));
        }
    }

    bool array(JValue& v, int depth) {
        v.type = JValue::Array;
        ++i_;
        skip();
        if (i_ < n_ && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            skip();
            JValue item;
            if (!value(item, depth + 1)) return false;
            v.items.push_back(std::move(item));
            skip();
            if (i_ < n_ && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < n_ && s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail(QStringLiteral("expected ',' or ']'"));
        }
    }

    bool string(QString& out) {
        ++i_;  // opening quote
        out.clear();
        while (i_ < n_) {
            const QChar c = s_[i_++];
            if (c == '"') return true;
            if (c == '\n') return fail(QStringLiteral("string not closed on its line"));
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= n_) break;
            const QChar e = s_[i_++];
            switch (e.unicode()) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i_ + 4 > n_) return fail(QStringLiteral("incomplete \\u escape"));
                    bool ok = false;
                    const ushort code = s_.mid(i_, 4).toUShort(&ok, 16);
                    if (!ok) return fail(QStringLiteral("invalid \\u escape"));
                    out += QChar(code);
                    i_ += 4;
                    break;
                }
                default: return fail(QStringLiteral("invalid escape"));
            }
        }
        return fail(QStringLiteral("string not closed"));
    }

    bool number(JValue& v) {
        const int start = i_;
        if (s_[i_] == '-') ++i_;
        while (i_ < n_ && (s_[i_].isDigit() || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' ||
                           ((s_[i_] == '+' || s_[i_] == '-') && (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E'))))
            ++i_;
        bool ok = false;
        v.type = JValue::Number;
        v.number = QStringView(s_).mid(start, i_ - start).toDouble(&ok);  // independent of the locale
        if (!ok) {
            i_ = start;
            return fail(QStringLiteral("invalid number"));
        }
        return true;
    }

    bool word(JValue& v) {
        const auto take = [&](const char* w) {
            const QString word = QString::fromLatin1(w);
            if (QStringView(s_).mid(i_, word.size()) != word) return false;
            i_ += int(word.size());
            return true;
        };
        if (take("true")) {
            v.type = JValue::Bool;
            v.boolean = true;
            return true;
        }
        if (take("false")) {
            v.type = JValue::Bool;
            return true;
        }
        if (take("null")) {
            v.type = JValue::Null;
            return true;
        }
        return fail(QStringLiteral("unexpected character '%1'").arg(s_[i_]));
    }

    const QString& s_;
    int n_;
    int i_ = 0;
    bool failed_ = false;
};

bool onlySpaces(const QString& text, int from, int to) {
    for (int k = from; k < to; ++k)
        if (text[k] != ' ' && text[k] != '\t') return false;
    return true;
}

int lineStart(const QString& text, int pos) {
    while (pos > 0 && text[pos - 1] != '\n') --pos;
    return pos;
}

int lineEnd(const QString& text, int pos) {
    while (pos < text.size() && text[pos] != '\n') ++pos;
    return pos;
}

// Position after the comma that follows position pos (only whitespace and
// comments in between), or -1.
int commaAfter(const QString& text, int pos, int limit) {
    for (int k = pos; k < limit; ++k) {
        const QChar c = text[k];
        if (c == ',') return k;
        if (c == '/' && k + 1 < limit && text[k + 1] == '/') {
            k = lineEnd(text, k);
        } else if (c == '/' && k + 1 < limit && text[k + 1] == '*') {
            const int close = int(text.indexOf(QStringLiteral("*/"), k + 2));
            if (close < 0) return -1;
            k = close + 1;
        } else if (!c.isSpace()) {
            return -1;
        }
    }
    return -1;
}

// The rest of the line from pos holds only spaces or a // comment.
bool restOfLineFree(const QString& text, int pos) {
    const int end = lineEnd(text, pos);
    for (int k = pos; k < end; ++k) {
        if (text[k] == ' ' || text[k] == '\t' || text[k] == '\r') continue;
        return text[k] == '/' && k + 1 < end && text[k + 1] == '/';
    }
    return true;
}

// Removes an element [begin, end) of a list (array item or object member)
// whose neighbours start at prevEnd (end of the previous element, -1 if
// none) and nextBegin (-1 if none); open/close are the list's brackets.
QString removeElement(const QString& text, int begin, int end, int prevEnd, int nextBegin, int open, int close) {
    QString out = text;
    if (nextBegin >= 0) {
        // remove the element and its comma; whole lines if it stands alone
        const int comma = commaAfter(text, end, nextBegin);
        int from = begin, to = comma >= 0 ? comma + 1 : nextBegin;
        const int ls = lineStart(text, begin);
        if (onlySpaces(text, ls, begin) && restOfLineFree(text, to)) {
            from = ls;
            to = lineEnd(text, to) + 1;
            // keep a comment that stood after the comma? It belongs to the element.
        } else {
            while (to < nextBegin && (text[to] == ' ' || text[to] == '\t')) ++to;
        }
        out.remove(from, to - from);
        return out;
    }
    if (prevEnd >= 0) {
        // the last element: remove the comma before it
        const int comma = commaAfter(text, prevEnd, begin);
        const int from = comma >= 0 ? comma : prevEnd;
        int to = end;
        const int ls = lineStart(text, begin);
        if (onlySpaces(text, ls, begin) && restOfLineFree(text, end) && lineEnd(text, end) < close) {
            // the element had a line of its own (and the bracket is on a later line)
            to = lineEnd(text, end);
            out.remove(from, to - from);
            // the comma stood at the end of the previous line: the line break stays
            return out;
        }
        out.remove(from, to - from);
        return out;
    }
    // the only element
    out.remove(open + 1, close - open - 1);
    return out;
}

}  // namespace

QString JValue::text(const QString& source) const {
    if (type == String) return string;
    if (type == Number) return source.mid(begin, end - begin);
    if (type == Bool) return boolean ? QStringLiteral("true") : QStringLiteral("false");
    return QString();
}

bool parseJsonc(const QString& text, JValue& root, QString* error, int* errorPos) {
    Parser p(text);
    root = JValue();
    if (p.parse(root)) return true;
    if (error) *error = p.error;
    if (errorPos) *errorPos = p.errorPos;
    return false;
}

QString jsonQuote(const QString& s) {
    QString out;
    out.reserve(s.size() + 2);
    out += '"';
    for (const QChar c : s) {
        switch (c.unicode()) {
            case '"': out += QStringLiteral("\\\""); break;
            case '\\': out += QStringLiteral("\\\\"); break;
            case '\n': out += QStringLiteral("\\n"); break;
            case '\r': out += QStringLiteral("\\r"); break;
            case '\t': out += QStringLiteral("\\t"); break;
            default:
                if (c.unicode() < 0x20) out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QChar('0'));
                else out += c;
        }
    }
    out += '"';
    return out;
}

QString indentationAt(const QString& text, int pos) {
    const int ls = lineStart(text, pos);
    int k = ls;
    while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) ++k;
    return text.mid(ls, k - ls);
}

QString replaceValue(const QString& text, const JValue& v, const QString& json) {
    QString out = text;
    out.replace(v.begin, v.end - v.begin, json);
    return out;
}

QString setMember(const QString& text, const JValue& obj, const QString& key, const QString& json) {
    const int k = obj.indexOf(key);
    if (k >= 0) return replaceValue(text, obj.items[size_t(k)], json);
    QString out = text;
    const QString member = jsonQuote(key) + QStringLiteral(": ") + json;
    if (obj.items.empty()) {
        out.replace(obj.begin, obj.end - obj.begin, QStringLiteral("{ ") + member + QStringLiteral(" }"));
        return out;
    }
    const JValue& last = obj.items.back();
    const bool oneLine = !QStringView(text).mid(obj.begin, obj.end - obj.begin).contains('\n');
    if (oneLine || lineStart(text, obj.keyBegins.back()) == lineStart(text, obj.begin)) {
        out.insert(last.end, QStringLiteral(", ") + member);
    } else {
        // a line of its own, indented like the last member; a comment after
        // the last value stays on its line
        const QString indent = indentationAt(text, obj.keyBegins.back());
        const int at = restOfLineFree(text, last.end) ? lineEnd(text, last.end) : last.end;
        out.insert(at, QStringLiteral("\n") + indent + member);
        out.insert(last.end, QStringLiteral(","));
    }
    return out;
}

QString removeMember(const QString& text, const JValue& obj, const QString& key) {
    const int k = obj.indexOf(key);
    if (k < 0) return text;
    const int n = int(obj.items.size());
    const int prevEnd = k > 0 ? obj.items[size_t(k - 1)].end : -1;
    const int nextBegin = k + 1 < n ? obj.keyBegins[size_t(k + 1)] : -1;
    return removeElement(text, obj.keyBegins[size_t(k)], obj.items[size_t(k)].end, prevEnd, nextBegin, obj.begin,
                         obj.end - 1);
}

QString removeItem(const QString& text, const JValue& arr, int index) {
    const int n = int(arr.items.size());
    if (index < 0 || index >= n) return text;
    const int prevEnd = index > 0 ? arr.items[size_t(index - 1)].end : -1;
    const int nextBegin = index + 1 < n ? arr.items[size_t(index + 1)].begin : -1;
    return removeElement(text, arr.items[size_t(index)].begin, arr.items[size_t(index)].end, prevEnd, nextBegin,
                         arr.begin, arr.end - 1);
}

QString appendItems(const QString& text, const JValue& arr, const QStringList& items) {
    if (items.isEmpty()) return text;
    QString out = text;
    if (arr.items.empty()) {
        const QString outer = indentationAt(text, arr.begin);
        const QString inner = outer + QStringLiteral("  ");
        out.replace(arr.begin, arr.end - arr.begin,
                    QStringLiteral("[\n") + inner + items.join(QStringLiteral(",\n") + inner) + QStringLiteral("\n") +
                        outer + QStringLiteral("]"));
        return out;
    }
    const JValue& last = arr.items.back();
    if (lineStart(text, last.begin) == lineStart(text, arr.begin)) {
        // a list on one line, as ["a", "b"]: stays on its line
        out.insert(last.end, QStringLiteral(", ") + items.join(QStringLiteral(", ")));
        return out;
    }
    const bool ownLine = onlySpaces(text, lineStart(text, last.begin), last.begin);
    const QString indent = ownLine ? indentationAt(text, last.begin) : indentationAt(text, arr.begin) + QStringLiteral("  ");
    const QString joined = items.join(QStringLiteral(",\n") + indent);
    const int at = restOfLineFree(text, last.end) && lineEnd(text, last.end) < arr.end - 1 ? lineEnd(text, last.end)
                                                                                            : last.end;
    out.insert(at, QStringLiteral("\n") + indent + joined);
    out.insert(last.end, QStringLiteral(","));
    return out;
}
