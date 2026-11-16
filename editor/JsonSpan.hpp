#pragma once

#include <QString>
#include <QStringList>
#include <vector>

// A JSON value read from text with // and /* */ comments, together with the
// place where it stands in the text. The schematic changes single values in
// place (the nodes of a component, one parameter, the "schematic" section)
// and leaves everything else - comments, layout, order - as it was.
//
// Positions are indices into the QString (UTF-16 code units), the same as
// the positions of a QTextDocument holding that text.
struct JValue {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    int begin = 0, end = 0;  // [begin, end) in the text
    QString string;          // String: the decoded text
    double number = 0.0;     // Number
    bool boolean = false;    // Bool
    // Array: the items. Object: the values, keys[k] belongs to items[k].
    std::vector<JValue> items;
    QStringList keys;
    std::vector<int> keyBegins;  // Object: position of the opening quote of each key

    bool isObject() const { return type == Object; }
    bool isArray() const { return type == Array; }
    bool isString() const { return type == String; }
    bool isNumber() const { return type == Number; }
    int indexOf(const QString& key) const { return type == Object ? keys.indexOf(key) : -1; }
    const JValue* get(const QString& key) const {
        const int k = indexOf(key);
        return k < 0 ? nullptr : &items[size_t(k)];
    }
    // A string, or a number written as it stands in the text, else empty.
    QString text(const QString& source) const;
};

// Parses JSON with comments. On failure returns false and sets the position
// and a description of the first error.
bool parseJsonc(const QString& text, JValue& root, QString* error = nullptr, int* errorPos = nullptr);

// "text" with quotes and escapes, as JSON.
QString jsonQuote(const QString& s);

// --- changes to the text ------------------------------------------------------
// Each function returns the changed text; the value positions refer to
// `text` as it was parsed.

// Replaces the value v by `json`.
QString replaceValue(const QString& text, const JValue& v, const QString& json);

// Sets member `key` of object `obj` to `json`, adding it after the last
// member if it is missing (on the same line for objects written on one line).
QString setMember(const QString& text, const JValue& obj, const QString& key, const QString& json);

// Removes member `key` of `obj` together with its comma; no change if missing.
QString removeMember(const QString& text, const JValue& obj, const QString& key);

// Removes item `index` of array `arr` together with its comma and its line,
// if it stood alone on that line.
QString removeItem(const QString& text, const JValue& arr, int index);

// Appends items (JSON texts) to array `arr`, each on a line of its own
// with the indentation of the existing items.
QString appendItems(const QString& text, const JValue& arr, const QStringList& items);

// Indentation (spaces and tabs) of the line that contains position pos.
QString indentationAt(const QString& text, int pos);
