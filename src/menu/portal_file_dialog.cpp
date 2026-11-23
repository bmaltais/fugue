#include "menu/portal_file_dialog.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QRegularExpression>

namespace fugue::portal {

void registerDBusTypes() {
    static const bool done = [] {
        qDBusRegisterMetaType<FilterRule>();
        qDBusRegisterMetaType<QList<FilterRule>>();
        qDBusRegisterMetaType<Filter>();
        qDBusRegisterMetaType<QList<Filter>>();
        return true;
    }();
    Q_UNUSED(done)
}

QDBusArgument &operator<<(QDBusArgument &arg, const FilterRule &rule) {
    arg.beginStructure();
    arg << rule.kind << rule.pattern;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, FilterRule &rule) {
    arg.beginStructure();
    arg >> rule.kind >> rule.pattern;
    arg.endStructure();
    return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const Filter &filter) {
    arg.beginStructure();
    arg << filter.name << filter.rules;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, Filter &filter) {
    arg.beginStructure();
    arg >> filter.name >> filter.rules;
    arg.endStructure();
    return arg;
}

QList<Filter> filtersFromNameFilters(const QStringList &nameFilters) {
    static const QRegularExpression named(QStringLiteral(R"(^(.*?)\s*\(([^()]*)\)\s*$)"));
    QList<Filter> out;
    for (const QString &text : nameFilters) {
        QString name = text.trimmed();
        QString globs = name;
        if (const auto match = named.match(name); match.hasMatch()) {
            name = match.captured(1).trimmed();
            globs = match.captured(2);
        }
        Filter filter;
        QStringList seen;
        for (const QString &glob : globs.split(QRegularExpression(QStringLiteral("[\\s;,]+")), Qt::SkipEmptyParts)) {
            // A glob has a wildcard or a dot; plain words are not patterns.
            if (!glob.contains('*') && !glob.contains('?') && !glob.startsWith('.'))
                continue;
            for (const QString &variant : {glob, glob.toUpper()}) {
                if (seen.contains(variant))
                    continue;
                seen << variant;
                filter.rules.append({0, variant});
            }
        }
        if (filter.rules.isEmpty())
            continue;
        filter.name = name.isEmpty() ? globs : name;
        out.append(filter);
    }
    return out;
}

QByteArray folderBytes(const QUrl &folder) {
    QByteArray bytes = QFile::encodeName(folder.toLocalFile());
    bytes.append('\0');
    return bytes;
}

QString methodFor(Mode mode) { return mode == Mode::SaveFile ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile"); }

QVariantMap options(const Request &request) {
    QVariantMap out;
    out.insert(QStringLiteral("handle_token"), request.token);
    out.insert(QStringLiteral("modal"), true);
    if (!request.filters.isEmpty()) {
        out.insert(QStringLiteral("filters"), QVariant::fromValue(request.filters));
        out.insert(QStringLiteral("current_filter"), QVariant::fromValue(request.filters.first()));
    }
    if (request.folder.isLocalFile() && !request.folder.toLocalFile().isEmpty())
        out.insert(QStringLiteral("current_folder"), folderBytes(request.folder));
    switch (request.mode) {
    case Mode::OpenFiles:
        out.insert(QStringLiteral("multiple"), true);
        break;
    case Mode::Directory:
        out.insert(QStringLiteral("directory"), true);
        break;
    case Mode::SaveFile:
        if (!request.name.isEmpty())
            out.insert(QStringLiteral("current_name"), request.name);
        break;
    case Mode::OpenFile:
        break;
    }
    return out;
}

QString requestPath(const QString &uniqueName, const QString &token) {
    QString sender = uniqueName;
    if (sender.startsWith(':'))
        sender.remove(0, 1);
    sender.replace('.', '_');
    return QStringLiteral("/org/freedesktop/portal/desktop/request/") + sender + '/' + token;
}

Response parseResponse(uint code, const QVariantMap &results) {
    Response out;
    if (code == 1) {
        out.status = Response::Cancelled;
        return out;
    }
    if (code != 0)
        return out;
    for (const QString &uri : results.value(QStringLiteral("uris")).toStringList()) {
        const QUrl url(uri);
        if (url.isLocalFile())
            out.urls.append(url);
    }
    if (out.urls.isEmpty())
        return out;
    out.status = Response::Accepted;
    const QVariant chosen = results.value(QStringLiteral("current_filter"));
    // Filters travel as a (sa(us)) structure; the name is its first member.
    if (chosen.canConvert<QDBusArgument>()) {
        Filter filter;
        chosen.value<QDBusArgument>() >> filter;
        out.filter = filter.name;
    }
    return out;
}

QUrl withDefaultSuffix(const QUrl &file, const QString &suffix) {
    if (suffix.isEmpty() || !file.isLocalFile())
        return file;
    const QString path = file.toLocalFile();
    if (path.endsWith('/') || !QFileInfo(path).suffix().isEmpty())
        return file;
    return QUrl::fromLocalFile(path + '.' + (suffix.startsWith('.') ? suffix.mid(1) : suffix));
}

} // namespace fugue::portal

namespace fugue {

bool PortalFileDialog::serviceAvailable() {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    const auto *interface = bus.interface();
    if (!interface)
        return false;
    const QString name = QStringLiteral("org.freedesktop.portal.Desktop");
    if (interface->isServiceRegistered(name).value())
        return true;
    // Not running yet, but the bus can start it (a .service file).
    return interface->activatableServiceNames().value().contains(name);
}

void PortalFileDialog::setSelectedFile(const QUrl &file) {
    m_files = file.isEmpty() ? QList<QUrl>{} : QList<QUrl>{file};
    emit selectedChanged();
}

// Stops listening for the answer: the request is over, whatever came of it.
void PortalFileDialog::stop() {
    QDBusConnection::sessionBus().disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath,
                                             QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this,
                                             SLOT(onResponse(uint, QVariantMap)));
    m_pending = false;
    m_requestPath.clear();
    emit visibleChanged();
}

void PortalFileDialog::fail(const QString &reason) {
    stop();
    emit failed(reason);
}

void PortalFileDialog::open() {
    if (m_pending)
        return;
    portal::registerDBusTypes();
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(QStringLiteral("no session bus"));
        return;
    }
    portal::Request request;
    request.mode = static_cast<portal::Mode>(m_mode);
    request.token = QStringLiteral("fugue_%1_%2").arg(QCoreApplication::applicationPid()).arg(++m_serial);
    request.filters = portal::filtersFromNameFilters(m_nameFilters);
    request.folder = m_folder;
    // A save starts with the name given, else the one of a selectedFile set beforehand.
    request.name = !m_suggestedName.isEmpty() ? m_suggestedName : (selectedFile().isLocalFile() ? selectedFile().fileName() : QString());
    m_files.clear();

    m_requestPath = portal::requestPath(bus.baseService(), request.token);
    // Subscribed before the call: the answer may come before the call's own reply.
    bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath, QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"), this, SLOT(onResponse(uint, QVariantMap)));

    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.FileChooser"), portal::methodFor(request.mode));
    // parent_window is "": no window handle is shared with the portal (xdg-foreign is not used).
    message << QString() << m_title << portal::options(request);
    m_pending = true;
    emit visibleChanged();
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, -1), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &PortalFileDialog::callFinished);
}

void PortalFileDialog::callFinished(QDBusPendingCallWatcher *watcher) {
    watcher->deleteLater();
    if (!m_pending)
        return;
    QDBusPendingReply<QDBusObjectPath> reply = *watcher;
    if (reply.isError()) {
        fail(reply.error().name() + ": " + reply.error().message());
        return;
    }
    // A portal that picks another handle than the one expected: listen where it says.
    const QString actual = reply.value().path();
    if (actual != m_requestPath) {
        auto bus = QDBusConnection::sessionBus();
        bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath, QStringLiteral("org.freedesktop.portal.Request"),
                       QStringLiteral("Response"), this, SLOT(onResponse(uint, QVariantMap)));
        m_requestPath = actual;
        bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_requestPath, QStringLiteral("org.freedesktop.portal.Request"),
                    QStringLiteral("Response"), this, SLOT(onResponse(uint, QVariantMap)));
    }
}

void PortalFileDialog::close() {
    if (!m_pending)
        return;
    stop();
}

void PortalFileDialog::onResponse(uint code, const QVariantMap &results) {
    if (!m_pending)
        return;
    stop();
    const portal::Response response = portal::parseResponse(code, results);
    switch (response.status) {
    case portal::Response::Cancelled:
        emit rejected();
        return;
    case portal::Response::Failed:
        emit failed(QStringLiteral("the file chooser portal answered with an error"));
        return;
    case portal::Response::Accepted:
        break;
    }
    m_files.clear();
    for (const QUrl &url : response.urls)
        m_files.append(m_mode == SaveFile ? portal::withDefaultSuffix(url, m_suffix) : url);
    emit selectedChanged();
    emit accepted();
}

} // namespace fugue
