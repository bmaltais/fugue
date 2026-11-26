#pragma once

// The first-run model folder setup (spec 8.9, #619), as the sheet shows it: a consent screen (why, every place Fugue would look
// with a tick each, then Search, Add a folder… or Skip), and only after Search the results (what was found, a tick each, the folder
// downloads go in, Use N folders). The finder (engine/model_folder_finder.h) runs on a worker thread and streams its folders here;
// nothing is written until the person says yes, and then in one settings write (SettingsModel::applyModelSetup).
//
// When it opens by itself (maybeOffer): once, in the Default session or a root with no project.json, with no model_paths, no model
// file in Fugue's own models folder and `models_setup` unset. Never inside a project (a rerun from Settings > Folders is the way
// there; it writes only the project's own file). Skip, Escape or closing writes `models_setup: "skipped"`; finishing writes "done";
// a rerun writes no `models_setup`.

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>
#include <functional>
#include <memory>

#include "engine/model_folder_finder.h"

namespace fugue {

class Registry;
class SettingsModel;

class ModelSetup : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App.settings")
    Q_PROPERTY(bool open READ isOpen NOTIFY changed)
    Q_PROPERTY(bool firstRun READ firstRun NOTIFY changed)
    // "consent", "looking" (results coming in), "results" (done looking, or none was made).
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(bool looking READ looking NOTIFY changed)
    Q_PROPERTY(bool nothing READ nothing NOTIFY changed) // looked, and found no folder to offer
    // The search stopped at a cap (time) before it was done: what is listed may be missing folders. The sheet says so.
    Q_PROPERTY(bool searchCapped READ searchCapped NOTIFY changed)
    Q_PROPERTY(bool inProject READ inProject NOTIFY changed)
    // [{id, display, hint, checked}], the places the search would look; only those that exist.
    Q_PROPERTY(QVariantList locations READ locations NOTIFY changed)
    Q_PROPERTY(bool canSearch READ canSearch NOTIFY changed)
    // [{path, display, title, hint, hint2, tooltip, on, inUse, inherited, readOnly, needsKind, kind, capped}]
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    // The folders new downloads may go in, {value, text, free, low}: Fugue's own and every picked writable layout folder.
    Q_PROPERTY(QVariantList choices READ choices NOTIFY changed)
    Q_PROPERTY(QString download READ download NOTIFY changed)
    Q_PROPERTY(QString downloadHint READ downloadHint NOTIFY changed)
    Q_PROPERTY(bool downloadLow READ downloadLow NOTIFY changed)
    // "Only 12 GB free there." when the chosen folder is short of room (kept chosen: never switched silently); else empty.
    Q_PROPERTY(QString downloadWarning READ downloadWarning NOTIFY changed)
    Q_PROPERTY(QString primaryText READ primaryText NOTIFY changed)
    Q_PROPERTY(bool primaryEnabled READ primaryEnabled NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)

public:
    explicit ModelSetup(SettingsModel *settings);
    ~ModelSetup() override;

    // What a worker needs, as values (it never reads a Qt object of the window: no registry, no settings model, not `this`).
    struct FinderJob {
        QList<FinderLocation> locations;      // the ticked places
        QList<FinderLocation> offered;        // every place shown: the ones not ticked are never touched
        QStringList skip;                     // folders already listed
        QStringList describe;                 // folders to count first (in use, or just picked)
        FinderEnv env;
        FinderTypes types;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    // The finder a job runs: on a worker thread, calling `found` with each folder; the result says whether a cap ended the search
    // early. The tests give a fake, and may run it inline.
    using Finder = std::function<FindResult(const FinderJob &, const std::function<void(const FoundFolder &)> &)>;
    void setFinder(Finder finder, bool runInline = false);
    // The world the finder reads (the tests' scratch home); default FinderEnv::system().
    void setEnv(const FinderEnv &env) { m_env = env; m_envSet = true; }
    // Free bytes of the disk a folder is on (QStorageInfo's by default); the tests give a made-up one.
    void setFreeSpace(std::function<qint64(const QString &)> free) { m_free = std::move(free); }

    bool isOpen() const { return m_open; }
    bool firstRun() const { return m_firstRun; }
    QString phase() const { return m_phase; }
    bool looking() const { return m_looking; }
    bool nothing() const;
    bool searchCapped() const { return m_capped && m_phase == QLatin1String("results"); }
    bool inProject() const;
    QVariantList locations() const;
    bool canSearch() const;
    QVariantList rows() const;
    QVariantList choices() const;
    QString download() const;
    QString downloadHint() const;
    bool downloadLow() const;
    QString downloadWarning() const;
    QString primaryText() const;
    bool primaryEnabled() const;
    QString message() const { return m_message; }

    // True when the setup should open by itself now (the four conditions above).
    Q_INVOKABLE bool shouldOffer() const;
    // Opens it when shouldOffer(); true when it did.
    Q_INVOKABLE bool maybeOffer();
    // Opens the consent screen. `firstRun` makes Skip and Done write `models_setup`; a rerun (false) never does.
    Q_INVOKABLE void show(bool firstRun);
    Q_INVOKABLE void setLocationChecked(const QString &id, bool checked);
    // Starts the search of the ticked places. Nothing else is touched.
    Q_INVOKABLE void search();
    // A folder the person picks: it goes straight to the results, with no search. "" or why not.
    Q_INVOKABLE QString addFolder(const QVariant &path);
    Q_INVOKABLE void setRowOn(const QString &path, bool on);
    Q_INVOKABLE void setRowKind(const QString &path, const QString &kind);
    Q_INVOKABLE void setDownload(const QString &path);
    // The kinds a folder can be set to hold, {value, text}, without "found by its files".
    Q_INVOKABLE QVariantList kinds() const;
    // Back to the consent screen with the same places ticked, to switch off the big ones and search again.
    Q_INVOKABLE void searchAgain();
    // Skip, Escape or the close: a first run writes "skipped", a rerun writes nothing; the sheet closes.
    Q_INVOKABLE void skip();
    // The primary: Use N folders (one settings write), or Done when nothing was found.
    Q_INVOKABLE void apply();

signals:
    void changed();
    // The sheet closed after a yes: the notice to say, and that the folders changed.
    void applied(const QString &notice);
    void closed();

private:
    struct Row {
        QString path;       // as shown and written, absolute
        FoundFolder found;
        bool on = false;
        bool inUse = false;
        bool inherited = false;
        bool writable = true;
        bool busy = false;  // being counted on the worker
        QString kind;       // "layout", a type id, or "" (not told: the person chooses)
    };

    void close();
    void begin(const QList<FinderLocation> &locations);
    void run(FinderJob job, bool search);
    void found(int generation, const FoundFolder &folder);
    void finished(int generation, bool capped);
    void addInUse();
    Row makeRow(const FoundFolder &folder) const;
    QList<int> order() const;
    QString ownRoot() const;
    QString defaultDownload() const;
    bool slowToWriteTo(const Row &row) const;
    const Registry *types() const;
    FinderEnv env() const;
    FinderJob makeJob() const;
    QVariantList foldersToWrite(int *left) const;

    SettingsModel *m_settings;
    Finder m_finder;
    bool m_inline = false;
    FinderEnv m_env;
    bool m_envSet = false;
    std::function<qint64(const QString &)> m_free;
    bool m_open = false;
    bool m_firstRun = false;
    QString m_phase = QStringLiteral("consent");
    bool m_looking = false;
    bool m_capped = false;
    QList<FinderLocation> m_locations;
    QStringList m_unticked; // ids
    QList<Row> m_rows;
    QString m_download;
    bool m_downloadChosen = false;
    QString m_message;
    int m_generation = 0;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false); // each job holds it: no wait on a worker
    FinderEnv m_envNow;   // the mounts as they were when the sheet opened, for what the window checks
};

} // namespace fugue
