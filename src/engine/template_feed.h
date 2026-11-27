#pragma once

// New templates without a release (spec 11.10, #699): the feed store fetches bmaltais/fugue-workflows `main` on
// its own, unpacks it and switches to it, and TemplateLibrary::standardRoots() lists its folder as a root
// between the person's own and the bundled ones.
//
// Only what changed is fetched. `GET .../commits/main` with `Accept: application/vnd.github.sha` is 40 bytes and
// ends the check when it is the sha in use. Otherwise `GET .../git/trees/<sha>?recursive=1` lists every file with
// its git blob sha; each file whose blob sha the base copy (the fetched folder in use, else the bundled templates)
// already holds is copied (a hard link when it can be), and the others come from raw.githubusercontent.com/.../<sha>/<path>
// and are refused unless their blob sha is the tree's. The new copy is built in a staging folder beside the store and
// named into place as <dataDir>/templates-online/<sha>/, with a `current` file naming the sha; older sha folders go
// after the switch. A truncated tree is refused, and so is a symlink, a submodule or a name outside the folder.
// Everything is HTTPS (plain http only to the loopback test address, below). Nothing from the feed runs: a pack
// a template carries still goes through the install and Trust questions.
//
// A failure (offline, GitHub's rate limit of 60 an hour without a key, a bad zip, a refused entry) keeps what is
// there, logs one line and is kept in the status (state folder, template-feed.json) with the time of the try.
//
// $FUGUE_TEMPLATE_FEED replaces the three addresses with one base for tests: <base>/commits/main,
// <base>/git/trees/<sha>?recursive=1 and <base>/raw/<sha>/<path>
// (http only to 127.0.0.1, localhost or ::1). $FUGUE_TEMPLATES set means the feed is never used (dev and tests).

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace fugue {

struct FeedResult {
    enum class Outcome { Updated, Current, Failed };
    Outcome outcome = Outcome::Failed;
    QString sha;      // the commit now in use (empty when there is none)
    QString previous; // the commit in use before this check
    QString error;    // one sentence, for Failed
    int downloaded = 0; // files fetched (the rest were copied from the base)
    int files = 0;      // files of the new copy
};

// What the last try left, for Settings > General ("Last checked").
struct FeedStatus {
    QDateTime checked; // invalid when it was never tried
    QString error;     // empty when the last try worked
    QString sha;
};

class TemplateFeed : public QObject {
    Q_OBJECT
public:
    explicit TemplateFeed(QNetworkAccessManager *network, QObject *parent = nullptr);

    // $FUGUE_TEMPLATES is set: this session reads its own templates folder and never the feed.
    static bool disabledByEnvironment();
    // The folder of the commit in use, or empty when none was fetched (or its folder is gone).
    static QString currentRoot();
    static QString currentSha();
    static QString storeDir();
    static FeedStatus status();
    // At most one automatic check in 6 hours, the time of the last try kept in the state folder.
    static bool due(const QDateTime &now = QDateTime::currentDateTimeUtc());

    bool busy() const { return m_busy; }
    // Starts a check and returns; finished() follows once. Does nothing while one is running.
    void check();
    // The CLI's: a check that returns when it is done.
    static FeedResult checkNow();
    // Where the first update copies from (the bundled templates by default; a test names its own).
    void setBundledRoot(const QString &path) { m_bundled = path; }

signals:
    void finished(const fugue::FeedResult &result);

private:
    struct Item {
        QString path;
        QString sha;
        qint64 size = 0;
        bool executable = false;
    };
    void get(const QUrl &url, const QByteArray &accept, void (TemplateFeed::*then)());
    void gotSha();
    void gotTree();
    void nextDownload();
    void gotFile();
    void fail(const QString &why);
    void finish();
    void done(FeedResult result);

    QNetworkAccessManager *m_network;
    QNetworkReply *m_reply = nullptr;
    QString m_bundled;
    QString m_previous;
    QString m_sha;
    QString m_staging;
    QList<Item> m_wanted; // still to download
    int m_files = 0;
    int m_downloaded = 0;
    bool m_busy = false;
};

} // namespace fugue
