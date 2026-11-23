#pragma once

// The desktop's own file picker, asked for over D-Bus: org.freedesktop.portal.FileChooser
// (OpenFile, SaveFile). On Omarchy the platform theme (qtengine) has no native file dialog, so
// Qt Quick's FileDialog would always fall back to its own QML one; the portal is what the
// desktop's file-picker backend answers (~/.config/xdg-desktop-portal/*portals.conf).
//
// The QML surface is that of QtQuick.Dialogs FileDialog as Fugue uses it (title, fileMode,
// currentFolder, nameFilters, defaultSuffix, selectedFile, selectedFiles, open(), close(),
// accepted, rejected), plus `suggestedName`, `available` and the `failed` signal. QML does not
// use it directly but through FilePicker.qml, which falls back to Qt's dialog when `available`
// is false or the call fails.
//
// Flow (the portal's Request protocol): the Response signal of the Request object is
// subscribed first (its path is known from our unique name and a handle_token), then the method
// is called; response 0 is accepted (`uris` are file:// URIs), 1 is cancelled, 2 is anything
// else (failed). A call error (no portal on the bus, an error reply) is `failed` too.
//
// The pure parts (the filter conversion, the options, the response) are free functions in
// `fugue::portal`, tested without a bus (tests/workflow/test_portal_file_dialog.cpp).

#include <QDBusArgument>
#include <QDBusPendingCallWatcher>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue::portal {

// One pattern of a filter: kind 0 is a glob ("*.png"), 1 a MIME type. a(us) in the portal's signature.
struct FilterRule {
    uint kind = 0;
    QString pattern;
};
// A filter: its name and its patterns, (sa(us)).
struct Filter {
    QString name;
    QList<FilterRule> rules;
    bool operator==(const Filter &other) const {
        return name == other.name && rules.size() == other.rules.size()
               && std::equal(rules.begin(), rules.end(), other.rules.begin(),
                             [](const FilterRule &a, const FilterRule &b) { return a.kind == b.kind && a.pattern == b.pattern; });
    }
};

// Makes the filter types known to QtDBus (once).
void registerDBusTypes();
QDBusArgument &operator<<(QDBusArgument &arg, const FilterRule &rule);
const QDBusArgument &operator>>(const QDBusArgument &arg, FilterRule &rule);
QDBusArgument &operator<<(QDBusArgument &arg, const Filter &filter);
const QDBusArgument &operator>>(const QDBusArgument &arg, Filter &filter);

// Qt-style name filters ("Fugue workflows (*.fugue)", "All files (*)", or a bare "*.png *.jpg") as
// the portal's filters. Globs are case-sensitive there, so each lowercase glob also gets its
// uppercase twin ("*.PNG"). A string with no usable glob is dropped.
QList<Filter> filtersFromNameFilters(const QStringList &nameFilters);

// The folder as the portal takes it: a NUL-terminated byte array of the local path.
QByteArray folderBytes(const QUrl &folder);

enum class Mode { OpenFile, OpenFiles, SaveFile, Directory };

struct Request {
    Mode mode = Mode::OpenFile;
    QString token;           // handle_token
    QList<Filter> filters;
    QUrl folder;             // current_folder, when a local folder
    QString name;            // current_name, save only
};
// The `options` of OpenFile or SaveFile.
QVariantMap options(const Request &request);
// "OpenFile" or "SaveFile".
QString methodFor(Mode mode);

// The Request object's path for our connection name (":1.42") and token.
QString requestPath(const QString &uniqueName, const QString &token);

struct Response {
    enum Status { Accepted, Cancelled, Failed } status = Failed;
    QList<QUrl> urls;
    QString filter; // the name of the filter the person ended on, "" when not told
};
// The Response signal's arguments. An accepted answer with no usable URI is Failed.
Response parseResponse(uint code, const QVariantMap &results);

// A save name with no extension gets the default suffix; a name with one is untouched.
QUrl withDefaultSuffix(const QUrl &file, const QString &suffix);

} // namespace fugue::portal

Q_DECLARE_METATYPE(fugue::portal::FilterRule)
Q_DECLARE_METATYPE(fugue::portal::Filter)

namespace fugue {

class PortalFileDialog : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString title MEMBER m_title NOTIFY changed)
    Q_PROPERTY(FileMode fileMode MEMBER m_mode NOTIFY changed)
    Q_PROPERTY(QUrl currentFolder MEMBER m_folder NOTIFY changed)
    Q_PROPERTY(QStringList nameFilters MEMBER m_nameFilters NOTIFY changed)
    Q_PROPERTY(QString defaultSuffix MEMBER m_suffix NOTIFY changed)
    // The name a save starts with. A `selectedFile` set before open() names it too.
    Q_PROPERTY(QString suggestedName MEMBER m_suggestedName NOTIFY changed)
    Q_PROPERTY(QUrl selectedFile READ selectedFile WRITE setSelectedFile NOTIFY selectedChanged)
    Q_PROPERTY(QList<QUrl> selectedFiles READ selectedFiles NOTIFY selectedChanged)
    // The chosen folder in Directory mode (FolderDialog's name for it).
    Q_PROPERTY(QUrl selectedFolder READ selectedFile WRITE setSelectedFile NOTIFY selectedChanged)
    // True when the portal's service is on the session bus (or can be started by it).
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool visible READ visible NOTIFY visibleChanged)

public:
    // The numbers of QtQuick.Dialogs FileDialog.fileMode, then the folder mode.
    enum FileMode { OpenFile, OpenFiles, SaveFile, Directory };
    Q_ENUM(FileMode)

    explicit PortalFileDialog(QObject *parent = nullptr) : QObject(parent) {}

    static bool serviceAvailable();
    bool available() const { return serviceAvailable(); }
    bool visible() const { return m_pending; }
    QUrl selectedFile() const { return m_files.isEmpty() ? QUrl() : m_files.first(); }
    QList<QUrl> selectedFiles() const { return m_files; }
    void setSelectedFile(const QUrl &file);

    Q_INVOKABLE void open();
    // Forgets the answer: the picker itself belongs to the portal and is closed by its own Cancel.
    Q_INVOKABLE void close();

signals:
    void accepted();
    void rejected();
    // The portal could not be asked, or answered an error: the caller opens another dialog.
    void failed(const QString &reason);
    void changed();
    void selectedChanged();
    void visibleChanged();

private slots:
    void onResponse(uint code, const QVariantMap &results);

private:
    void finish();
    void callFinished(QDBusPendingCallWatcher *watcher);
    void stop();
    void fail(const QString &reason);

    QString m_title;
    FileMode m_mode = OpenFile;
    QUrl m_folder;
    QStringList m_nameFilters;
    QString m_suffix;
    QString m_suggestedName;
    QList<QUrl> m_files;
    bool m_pending = false;
    QString m_requestPath;
    quint64 m_serial = 0;
};

} // namespace fugue
