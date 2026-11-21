#include "workflow/recent_files.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>

#include "engine/json.h"
#include "workflow/workflows.h"

namespace fugue {

RecentFiles::RecentFiles(QString path, int limit, QObject *parent)
    : QObject(parent), m_path(std::move(path)), m_limit(limit) {
    if (const Result<QJsonObject> read = readJsonObject(m_path))
        for (const QJsonValue &value : read->value("files").toArray())
            if (value.isString() && !value.toString().isEmpty() && !m_files.contains(value.toString()))
                m_files << value.toString();
    while (m_files.size() > m_limit)
        m_files.removeLast();
}

QVariantList RecentFiles::entries() const {
    QVariantList list;
    const QString home = QDir::homePath();
    for (const QString &path : m_files) {
        const QFileInfo info(path);
        if (m_open.contains(path) || !info.isFile())
            continue;
        QString folder = info.absolutePath();
        if (folder == home || folder.startsWith(home + QLatin1Char('/')))
            folder = QLatin1Char('~') + folder.mid(home.size());
        const QString name = info.fileName();
        list.append(QVariantMap{{"path", path},
                                {"name", name.endsWith(QLatin1String(".fugue")) ? name.chopped(6) : name},
                                {"folder", folder}});
    }
    return list;
}

void RecentFiles::remember(const QString &path) {
    if (path.trimmed().isEmpty() || (!m_files.isEmpty() && m_files.first() == path))
        return;
    m_files.removeAll(path);
    m_files.prepend(path);
    while (m_files.size() > m_limit)
        m_files.removeLast();
    save();
    emit entriesChanged();
}

void RecentFiles::setOpen(const QStringList &paths) {
    if (paths == m_open)
        return;
    m_open = paths;
    emit entriesChanged();
}

void RecentFiles::forget(const QString &path) {
    if (!m_files.removeAll(path))
        return;
    save();
    emit entriesChanged();
}

void RecentFiles::follow(Workflows *workflows) {
    const auto read = [this, workflows] {
        QStringList open;
        for (const QVariant &tab : workflows->tabs())
            if (const QString path = tab.toMap().value("path").toString(); !path.isEmpty())
                open << path;
        // The last tab first, so the first of several new tabs ends up first.
        for (int i = open.size() - 1; i >= 0; --i)
            if (!m_open.contains(open[i])) // newly open: opened, or saved under a new name
                remember(open[i]);
        setOpen(open);
    };
    connect(workflows, &Workflows::tabsChanged, this, read);
    read();
}

void RecentFiles::save() const {
    if (m_path.isEmpty())
        return;
    QJsonArray files;
    for (const QString &path : m_files)
        if (QFileInfo::exists(path))
            files.append(path);
    writeJsonFile(m_path, QJsonObject{{"files", files}});
}

} // namespace fugue
