#include "registry_reloader.h"

#include <QDir>
#include <QFileInfo>

#include "engine/pack_loader.h"
#include "engine/registry.h"

namespace fugue {

RegistryReloader::RegistryReloader(Registry *registry, Fill fill, std::function<QStringList()> folders, QObject *parent)
    : QObject(parent), m_registry(registry), m_fill(std::move(fill)),
      m_folders(folders ? std::move(folders) : std::function<QStringList()>(packSearchPaths)) {
    m_settle.setSingleShot(true);
    m_settle.setInterval(500);
    connect(&m_settle, &QTimer::timeout, this, [this] { load(false); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &RegistryReloader::reloadSoon);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &RegistryReloader::reloadSoon);
    watch();
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
    if (changed || always)
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
    for (const NodeType *type : m_registry->nodeTypes()) {
        const QString entry = type->runtime.value("entry").toString();
        if (!type->dir.isEmpty() && !entry.isEmpty())
            paths << QDir(type->dir).absoluteFilePath(entry);
    }
    paths.removeDuplicates();
    QStringList present;
    for (const QString &path : std::as_const(paths))
        if (QFileInfo::exists(path))
            present << path;
    if (!present.isEmpty())
        m_watcher.addPaths(present);
}

} // namespace fugue
