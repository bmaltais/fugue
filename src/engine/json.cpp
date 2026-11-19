#include "json.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace fugue {

namespace {

// A scalar as Qt writes it compactly: Qt's string escaping and its integer and
// shortest round-trip double forms.
QByteArray scalar(const QJsonValue &value) {
    QByteArray text = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return text.mid(1, text.size() - 2);
}

void write(QByteArray &out, const QJsonValue &value, int depth) {
    const QByteArray indent(2 * (depth + 1), ' ');
    const QByteArray closing(2 * depth, ' ');
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        bool first = true;
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!first)
                out += ",\n";
            first = false;
            out += indent + scalar(it.key()) + ": ";
            write(out, it.value(), depth + 1);
        }
        out += "\n" + closing + "}";
    } else if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (array.isEmpty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (i)
                out += ",\n";
            out += indent;
            write(out, array.at(i), depth + 1);
        }
        out += "\n" + closing + "]";
    } else {
        out += scalar(value);
    }
}

} // namespace

QByteArray writeJson(const QJsonValue &value) {
    QByteArray out;
    write(out, value, 0);
    out += '\n';
    return out;
}

} // namespace fugue
