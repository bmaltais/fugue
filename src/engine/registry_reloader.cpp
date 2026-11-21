#include "registry_reloader.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include "engine/pack_loader.h"
#include "engine/registry.h"

#include <utility>

namespace fugue {

RegistryReloader::RegistryReloader(Registry *registry, Fill fill, std::function<QStringList()> folders, QObject *parent)
    : QObject(parent), m_registry(registry), m_fill(std::move(fill)),
      m_folders(folders ? std::move(folders) : std::function<QStringList()>(packSearchPaths)) {
    m_settle.setSingleShot(true);
    m_settle.setInterval(500);
    connect(&m_settle, &QTimer::timeout, this, [this] { load(false); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &RegistryReloader::onDirectory);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &RegistryReloader::onFile);
    watch();
}

void RegistryReloader::setTrustFile(const QString &path) {
    m_trustFile = path;
    m_trustStamp = trustStamp();
    watch();
}

QString RegistryReloader::trustStamp() const {
    const QFileInfo info(m_trustFile);
    return info.exists() ? QStringLiteral("%1 %2").arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size()) : QString();
}

void RegistryReloader::onDirectory(const QString &path) {
    // The trust file's folder (the state folder) changes with every run: only the file counts.
    if (!m_trustFile.isEmpty() && path == QFileInfo(m_trustFile).absolutePath()) {
        trustTouched();
        // A pack path may be the same folder; it is filled again too then.
        if (!m_folders().contains(path))
            return;
    }
    reloadSoon();
}

void RegistryReloader::onFile(const QString &path) {
    if (path == m_trustFile)
        trustTouched();
    else
        reloadSoon();
}

void RegistryReloader::trustTouched() {
    const QString stamp = trustStamp();
    if (stamp == m_trustStamp)
        return;
    m_trustStamp = stamp;
    m_trustChanged = true;
    reloadSoon();
}

void RegistryReloader::reload() {
    m_settle.stop();
    load(true);
}

void RegistryReloader::reloadSoon() { m_settle.start(); }

void RegistryReloader::load(bool always) {
    Registry fresh;
    m_fill(fresh);
    const bool changed = !m_registry->sameTypes(fresh);
    if (changed)
        m_registry->replaceTypes(std::move(fresh));
    watch();
    m_trustStamp = trustStamp(); // a trust given here asked for this reload itself
    if (changed || always || std::exchange(m_trustChanged, false))
        emit reloaded();
}

void RegistryReloader::watch() {
    const QStringList old = m_watcher.files() + m_watcher.directories();
    if (!old.isEmpty())
        m_watcher.removePaths(old);
    QStringList paths;
    const auto subfolders = [](const QString &dir) {
        return QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    };
    for (const QString &root : m_folders()) {
        if (!QFileInfo(root).isDir()) {
            // Not made yet (no pack installed so far): the nearest folder there is sees it come.
            // Only entries of that folder itself fire, so a busy ~/.local/share costs little.
            QString up = QFileInfo(root).absolutePath();
            while (!QFileInfo(up).isDir() && QFileInfo(up).absolutePath() != up)
                up = QFileInfo(up).absolutePath();
            paths << up;
            continue;
        }
        paths << root;
        for (const QFileInfo &pack : subfolders(root)) {
            paths << pack.filePath() << pack.filePath() + QStringLiteral("/pack.json");
            for (const QFileInfo &node : subfolders(pack.filePath()))
                paths << node.filePath() << node.filePath() + QStringLiteral("/node.json");
        }
    }
    for (const NodeType *type : m_registry->allNodeTypes()) {
        const QString entry = type->runtime.value("entry").toString();
        if (!type->dir.isEmpty() && !entry.isEmpty())
            paths << QDir(type->dir).absoluteFilePath(entry);
    }
    if (!m_trustFile.isEmpty())
        paths << m_trustFile << QFileInfo(m_trustFile).absolutePath(); // the folder sees it made
    paths.removeDuplicates();
    QStringList present;
    for (const QString &path : std::as_const(paths))
        if (QFileInfo::exists(path))
            present << path;
    if (!present.isEmpty())
        m_watcher.addPaths(present);
}

} // namespace fugue
