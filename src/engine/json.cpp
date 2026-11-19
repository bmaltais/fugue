#include "json.h"

#include <QDir>
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

Errors writeJsonFile(const QString &path, const QJsonValue &value) {
    QSaveFile file(path);
    const auto failed = [&] {
        return Errors{makeError(QStringLiteral("permission_denied"),
                                QStringLiteral("%1 cannot be written: %2.").arg(path, file.errorString()))};
    };
    if (!QDir().mkpath(QFileInfo(path).path()) || !file.open(QIODevice::WriteOnly))
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
    QByteArray text = file.readAll();
    if (text.startsWith("\xef\xbb\xbf"))
        text = text.mid(3);
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(text, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        const QByteArray before = text.left(parseError.offset);
        const qsizetype line = before.count('\n') + 1;
        const qsizetype column = parseError.offset - (before.lastIndexOf('\n') + 1) + 1;
        return makeError(QStringLiteral("invalid_json"),
                         QStringLiteral("%1 is not valid JSON: %2 at line %3, column %4.")
                             .arg(path, parseError.errorString())
                             .arg(line)
                             .arg(column));
    }
    if (!json.isObject())
        return makeError(QStringLiteral("invalid_json"), QStringLiteral("%1 does not hold a JSON object.").arg(path));
    return json.object();
}

QByteArray writeJson(const QJsonValue &value) {
    QByteArray out;
    write(out, value, 0);
    out += '\n';
    return out;
}

} // namespace fugue
