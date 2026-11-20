#pragma once

// A pack on disk for the tests that run command nodes from the window: io.example.tools with one
// command node, such as `say` (#70), a one-shot program that answers with a text.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace pack_fixture {

inline bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(bytes);
    return true;
}

// Writes (or overwrites) `<packs>/io.example.tools` at `version` with one command node, `name`,
// whose program is `script` (after a bash #! line), with an optional text input `words` and a
// text output `text`, and the fields of `extra` added to its manifest. True when it was written.
inline bool writeToolsPack(const QString &packs, const QString &name, const QByteArray &script,
                           const QString &version = QStringLiteral("1.0.0"), const QJsonObject &extra = {}) {
    const QString dir = packs + QStringLiteral("/io.example.tools");
    const QJsonObject pack{{"manifest", 1}, {"id", "io.example.tools"}, {"version", version},
                           {"name", "Example tools"}, {"nodes", QJsonArray{name}}};
    QString title = name;
    title[0] = title[0].toUpper();
    QJsonObject node{{"manifest", 1},
                     {"id", "io.example.tools/" + name},
                     {"version", version},
                     {"name", title},
                     {"category", "Example"},
                     {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                     {"inputs", QJsonArray{QJsonObject{{"name", "words"}, {"type", "text"}, {"optional", true}}}},
                     {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        node.insert(it.key(), it.value());
    const QString nodeDir = dir + "/" + name;
    if (!writeFile(dir + "/pack.json", QJsonDocument(pack).toJson()) || !writeFile(nodeDir + "/node.json", QJsonDocument(node).toJson())
        || !writeFile(nodeDir + "/run.sh", "#!/bin/bash\n" + script))
        return false;
    return QFile::setPermissions(nodeDir + "/run.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// io.example.tools with `say`, which answers with the text `says`.
inline bool writeSayPack(const QString &packs, const QString &says, const QString &version = QStringLiteral("1.0.0")) {
    return writeToolsPack(packs, QStringLiteral("say"),
                          "cat > /dev/null\necho '{\"outputs\": {\"text\": \"" + says.toUtf8() + "\"}}'\n", version);
}

} // namespace pack_fixture
