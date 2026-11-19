#include "json.h"

#include <algorithm>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

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
        // Sorted here, by UTF-16 code unit as canonical JSON does (canonical.cpp), not
        // left to QJsonObject: Qt orders a parsed object's keys differently from an
        // inserted one's once a key at U+E000 or above meets one outside the basic plane.
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        out += "{\n";
        bool first = true;
        for (const QString &key : std::as_const(keys)) {
            if (!first)
                out += ",\n";
            first = false;
            out += indent + scalar(key) + ": ";
            write(out, object.value(key), depth + 1);
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

Errors writeJsonFile(const QString &path, const QJsonValue &value) {
    QSaveFile file(path);
    const auto failed = [&] {
        return Errors{makeError(QStringLiteral("permission_denied"),
                                QStringLiteral("%1 cannot be written: %2.").arg(path, file.errorString()))};
    };
    if (!file.open(QIODevice::WriteOnly))
        return failed();
    const QByteArray bytes = writeJson(value);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return failed();
    }
    if (!file.commit())
        return failed();
    return {};
}

Result<QJsonDocument> parseJson(const QByteArray &bytes, const QString &what) {
    const QByteArray text = bytes.startsWith("\xef\xbb\xbf") ? bytes.mid(3) : bytes;
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(text, &parseError);
    if (parseError.error == QJsonParseError::NoError)
        return json;
    const QByteArray before = text.left(parseError.offset);
    const qsizetype line = before.count('\n') + 1;
    const qsizetype column = parseError.offset - (before.lastIndexOf('\n') + 1) + 1;
    return makeError(QStringLiteral("invalid_json"),
                     QStringLiteral("%1 is not valid JSON: %2 at line %3, column %4.")
                         .arg(what, parseError.errorString())
                         .arg(line)
                         .arg(column));
}

Result<QJsonObject> readJsonObject(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || info.isDir())
        return makeError(QStringLiteral("file_not_found"),
                         info.isDir() ? QStringLiteral("%1 is a folder, not a file.").arg(path)
                                      : QStringLiteral("There is no file %1.").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return makeError(QStringLiteral("permission_denied"),
                         QStringLiteral("%1 cannot be read: %2.").arg(path, file.errorString()));
    const Result<QJsonDocument> json = parseJson(file.readAll(), path);
    if (!json)
        return json.error();
    if (!json->isObject())
        return makeError(QStringLiteral("invalid_json"), QStringLiteral("%1 does not hold a JSON object.").arg(path));
    return json->object();
}

QByteArray writeJson(const QJsonValue &value) {
    QByteArray out;
    write(out, value, 0);
    out += '\n';
    return out;
}

} // namespace fugue
