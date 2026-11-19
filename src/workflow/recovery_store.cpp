#include "workflow/recovery_store.h"

#include "engine/atomic_file.h"
#include "engine/json.h"
#include "engine/paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace fugue {

QString RecoveryStore::defaultDir() { return stateDir() + "/recovery"; }

RecoveryStore::RecoveryStore(QString dir) : m_dir(std::move(dir)) {}

bool RecoveryStore::validId(const QString &id) {
    if (id.isEmpty() || id.size() > 64)
        return false;
    return std::all_of(id.begin(), id.end(), [](QChar c) { return c.isLetterOrNumber() || c == '-' || c == '_'; });
}

QString RecoveryStore::copyPath(const QString &id) const { return m_dir + "/" + id + ".fugue"; }
QString RecoveryStore::metaPath(const QString &id) const { return m_dir + "/" + id + ".json"; }

Errors RecoveryStore::write(const QString &id, const Document &document, const QString &originalPath,
                            const QString &title) {
    if (!validId(id))
        return Errors{makeError("invalid_argument", QStringLiteral("%1 is not a recovery id.").arg(id))};
    if (const Errors made = makeFolder(m_dir); !made.isEmpty())
        return made;
    // The copy is a workflow file of its own; the original is never touched. Both files are
    // the owner's alone, as the work they hold may be private.
    if (const Errors saved = replaceFile(copyPath(id), writeJson(document.toJson()), QStringLiteral("the recovery copy"),
                                         FileMode::OwnerOnly);
        !saved.isEmpty())
        return saved;
    const QJsonObject meta{{"path", originalPath}, {"title", title}};
    return replaceFile(metaPath(id), QJsonDocument(meta).toJson(QJsonDocument::Compact),
                       QStringLiteral("the recovery note"), FileMode::OwnerOnly);
}

void RecoveryStore::remove(const QString &id) {
    if (!validId(id))
        return;
    QFile::remove(copyPath(id));
    QFile::remove(metaPath(id));
}

QString RecoveryStore::setAside(const QString &id) {
    if (!validId(id))
        return {};
    const QString aside = m_dir + "/unreadable";
    if (!makeFolder(aside).isEmpty())
        return {};
    // A copy set aside before under the same id keeps its place: this one takes another name.
    QString name = id + ".fugue";
    for (int n = 2; QFileInfo::exists(aside + "/" + name); ++n)
        name = QStringLiteral("%1-%2.fugue").arg(id).arg(n);
    if (!QFile::rename(copyPath(id), aside + "/" + name))
        return {};
    QFile::remove(metaPath(id));
    return aside;
}

QList<RecoveryEntry> RecoveryStore::list() const {
    QList<RecoveryEntry> entries;
    if (!QFileInfo(m_dir).isDir())
        return entries;
    PartialFile::removeStale(m_dir);
    for (const QFileInfo &info : QDir(m_dir).entryInfoList({"*.fugue"}, QDir::Files)) {
        RecoveryEntry entry;
        entry.id = info.completeBaseName();
        if (!validId(entry.id))
            continue;
        entry.modified = info.lastModified();
        QFile meta(metaPath(entry.id));
        if (meta.open(QIODevice::ReadOnly)) {
            const QJsonObject object = QJsonDocument::fromJson(meta.readAll()).object();
            entry.originalPath = object.value("path").toString();
            entry.title = object.value("title").toString();
        }
        if (entry.title.isEmpty())
            entry.title = entry.originalPath.isEmpty() ? entry.id : QFileInfo(entry.originalPath).fileName();
        entries.append(entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const RecoveryEntry &a, const RecoveryEntry &b) { return a.modified > b.modified; });
    return entries;
}

Result<Document> RecoveryStore::read(const QString &id) const {
    if (!validId(id))
        return makeError("invalid_argument", QStringLiteral("%1 is not a recovery id.").arg(id));
    return Document::loadFile(copyPath(id));
}

} // namespace fugue
