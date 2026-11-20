#pragma once

// A pack on disk for the window tests of live node types (#70): io.example.tools with one
// command node, `say`, a one-shot program that answers with the text `says` on its output.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace canvas_test {

inline bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(bytes);
    return true;
}

// Writes (or overwrites) `<packs>/io.example.tools` at `version`. True when it was written.
inline bool writeSayPack(const QString &packs, const QString &says, const QString &version = QStringLiteral("1.0.0")) {
    const QString dir = packs + QStringLiteral("/io.example.tools");
    const QJsonObject pack{{"manifest", 1}, {"id", "io.example.tools"}, {"version", version},
                           {"name", "Example tools"}, {"nodes", QJsonArray{"say"}}};
    const QJsonObject node{{"manifest", 1},
                           {"id", "io.example.tools/say"},
                           {"version", version},
                           {"name", "Say"},
                           {"category", "Example"},
                           {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                           {"inputs", QJsonArray{QJsonObject{{"name", "words"}, {"type", "text"}, {"optional", true}}}},
                           {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}}}}};
    const QByteArray script = "#!/bin/bash\ncat > /dev/null\necho '{\"outputs\": {\"text\": \"" + says.toUtf8() + "\"}}'\n";
    if (!writeFile(dir + "/pack.json", QJsonDocument(pack).toJson()) || !writeFile(dir + "/say/node.json", QJsonDocument(node).toJson())
        || !writeFile(dir + "/say/run.sh", script))
        return false;
    return QFile::setPermissions(dir + "/say/run.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

} // namespace canvas_test
