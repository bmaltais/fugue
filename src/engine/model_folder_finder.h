#pragma once

// Finding the model folders a person already has (spec 8.9, #619): the places ComfyUI, Forge,
// Automatic1111, SwarmUI, Stability Matrix and InvokeAI keep their models, found by folder names and
// file sizes alone. A model file is never opened, read or hashed, and nothing is written.
//
// Where it looks is a list of locations the person can tick (the first-run screen, `fugue model
// find-folders --location`): the fixed install folders that exist (~/ComfyUI/models and the like),
// then `~` (4 levels deep), `/mnt`, `/media/$USER` and `/run/media/$USER` (each drive 5 levels deep).
// A location that is not ticked is never touched: no stat, no listing, no walk, whatever the other
// locations hold. A folder is a candidate when it holds at least two of the resource types' folder
// names (the registry's, so the A1111, SwarmUI and Stability Matrix names count) and is told by what
// stands beside it (`custom_nodes/`, `webui.py`...).
//
// Caps: finding folders stops after 3 s in all; counting the models of one folder stops after 2 s or
// 20,000 entries (`countCapped`). Network file systems are never walked (an exact install folder on
// one is listed, not counted). All of it is `FinderEnv`, so a test injects the home, the mounts, the
// clock and a probe that sees every path touched.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <functional>

namespace fugue {

class Registry;

// What the finder needs of the registry, as plain values: the worker thread gets a copy made on the GUI thread, so a registry
// reload (RegistryReloader) cannot pull the types from under a walk.
struct FinderTypes {
    QHash<QString, QString> typeOfFolder;   // a type folder's name -> its resource type id
    QHash<QString, QStringList> extensions; // type id -> its file extensions (empty: ComfyUI's list plus .gguf)
    static FinderTypes from(const Registry &registry);
    bool takes(const QString &typeId, const QString &fileName) const;
};

// The world the finder reads. `system()` is this machine's.
struct FinderEnv {
    QString home;                 // `~`
    QStringList driveParents;     // the folders whose children are drives: /mnt, /media/$USER, /run/media/$USER
    QString ownDataDir;           // Fugue's own data folder: never walked
    QStringList networkMounts;    // mount points of network file systems and autofs points: never stat'd into, walked or counted
    QStringList autofsMounts;     // the autofs ones among them: not even listed (stat'ing into one mounts it)
    int findCapMs = 3000;         // finding the folders, in all
    int countCapMs = 2000;        // counting one folder
    int countCapEntries = 20000;  // entries looked at when counting one folder
    int totalCountCapMs = 10000;  // counting all folders, in all: the rest are listed with a count that stopped
    int tooManyEntries = 5000;    // a folder with more entries than this is not walked (a photo folder)
    std::function<qint64()> nowMs; // a monotonic clock in ms; empty: the real one
    std::function<void(const QString &)> touched; // called with every path stat'd or listed (a test's probe)
    const std::atomic<bool> *cancel = nullptr;    // true: stop as soon as possible

    static FinderEnv system();
    qint64 now() const;
};

// The mount points of the network file systems (nfs, cifs, sshfs...) and of autofs in a /proc/self/mountinfo. A path in one is never
// stat'd into: touching an automount mounts it.
QStringList networkMountsFrom(const QByteArray &mountinfo);
// Only the autofs ones.
QStringList autofsMountsFrom(const QByteArray &mountinfo);

// One place the person can allow or refuse.
struct FinderLocation {
    QString id;       // "probe:<path>", "home", "drive:<path>"; stable for a path
    QString path;     // as it is on this machine, absolute
    QString display;  // with ~ for the home
    int depth = 0;    // how deep the walk goes (0: an exact folder)
    QString scope;    // what the place covers: the folder itself, or an install folder's parent (its siblings name the tool)
};

struct FoundFolder {
    QString tool;            // "ComfyUI", "ComfyUI portable", "ComfyUI Desktop", "Forge", "Automatic1111", "SwarmUI",
                             // "Stability Matrix", "InvokeAI" or "Model folder"
    QString path;            // the folder as found first (a models/ folder), absolute
    QString canonical;       // its canonical path
    QStringList alsoReached; // other paths of the same folder, or of one that lives inside it
    QHash<QString, int> counts; // model files by resource type id
    int models = 0;
    qint64 size = 0;         // bytes of the files counted
    bool countCapped = false;// counting stopped at a cap: `models` and `size` are at least that
    bool network = false;    // on a network drive: listed, not counted
    bool layout = true;      // in ComfyUI's layout; false for InvokeAI (sorts its own way)
    bool writable = true;    // Fugue can write in it (access(), asked on the worker; a network drive is not asked)
    QStringList typeDirs;    // canonical paths of its type folders (for telling a link from a copy)
};

struct FindResult {
    QList<FoundFolder> folders; // in the order found
    bool capped = false;        // a cap (3 s for finding, or the total for counting) stopped the search: folders may be missing
};

// The locations to offer, in order. Existing ones only (one stat each; nothing below them is listed).
QList<FinderLocation> finderLocations(const FinderEnv &env);

// Looks in the ticked `locations` (those of finderLocations()). `found` is called for each folder as soon as it
// is counted. Reads no file. Fugue's own models folder and any folder in `skip` (canonical or plain paths) are left out.
// `offered` are the places the person was shown (finderLocations()); the ones of them not in `locations` are the ones left off.
// Empty: nothing was left off.
FindResult findModelFolders(const FinderTypes &types, const QList<FinderLocation> &locations, const FinderEnv &env,
                            const QStringList &skip = {}, const std::function<void(const FoundFolder &)> &found = {},
                            const QList<FinderLocation> &offered = {});
FindResult findModelFolders(const Registry &registry, const QList<FinderLocation> &locations, const FinderEnv &env,
                            const QStringList &skip = {}, const std::function<void(const FoundFolder &)> &found = {},
                            const QList<FinderLocation> &offered = {});

// A folder the person chose, counted the same way (a layout folder by its type folders, any other by all its model files).
FoundFolder describeModelFolder(const FinderTypes &types, const QString &path, const FinderEnv &env);
FoundFolder describeModelFolder(const Registry &registry, const QString &path, const FinderEnv &env);

// The tool of a models folder, from what stands beside it; "Model folder" for none. Reads the parent's listing.
QString finderToolName(const QString &modelsDir, const FinderEnv &env);

} // namespace fugue
