#pragma once

// Live node types (#70, spec 6.5): the window's registry is filled again from disk when the
// packs change, so what is installed, updated, removed or saved to the library is usable at
// once, with no restart. The registry stays where it is (Registry::replaceTypes); what holds a
// pointer to one of its types must take it again on reloaded(), when the old ones are gone.
//
// Two ways in:
//   - reload(): asked for (the Packs page installed, updated, removed or trusted a pack; a
//     group was saved to the library). At once, and reloaded() always follows, so what shows
//     trust is asked again too.
//   - the pack folders changing on disk (`fugue pack add` from the CLI, another window saving
//     to the library, an edit by hand): watched, and once they have been quiet for a moment
//     (setSettleMs) filled again; reloaded() only when the types are not the same as before.
//     reloadSoon() starts the same wait (pack_paths changed in the settings).
//
// What is watched: each pack search path (the nearest folder above it when it is not there
// yet), each pack folder and its pack.json, each node folder and its node.json, and the entry
// file of each command node (its seal covers it), and the trust file when one is set. The
// watch is made again after every fill, as a file replaced by a rename is a new file.

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace fugue {

class Registry;

class RegistryReloader : public QObject {
    Q_OBJECT

public:
    using Fill = std::function<void(Registry &)>;

    // `fill` puts every node type into an empty registry (built-ins, then the packs), as at
    // startup; `registry` is already filled. `folders` gives the pack search paths to watch, asked
    // again at each fill (default packSearchPaths()).
    RegistryReloader(Registry *registry, Fill fill, std::function<QStringList()> folders = {},
                     QObject *parent = nullptr);

    void reload();
    void reloadSoon();
    // How long the folders must be quiet before a fill (default 500 ms).
    void setSettleMs(int ms) { m_settle.setInterval(ms); }
    // trust.json (6.7), watched too: a pack trusted from outside the window (`fugue pack trust`)
    // is a reload, so what asks for trust asks again. Only the file's own change counts, not
    // another file of its folder.
    void setTrustFile(const QString &path);

signals:
    // The registry holds the types read now. Everything that shows node types refreshes.
    void reloaded();

private:
    void load(bool always);
    void watch();
    void onDirectory(const QString &path);
    void onFile(const QString &path);
    // The trust file changed since it was last seen: a reload, after the settle time.
    void trustTouched();
    QString trustStamp() const;

    Registry *m_registry;
    Fill m_fill;
    std::function<QStringList()> m_folders;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    QString m_trustFile;
    QString m_trustStamp; // its time and size when last seen
    bool m_trustChanged = false; // the next fill says reloaded() even when the types are the same
};

} // namespace fugue
