#include "engine/template_feed.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <filesystem>
#include <unistd.h>

#include "engine/fugue_home.h"
#include "engine/paths.h"
#include "engine/templates.h"

namespace fugue {

namespace {

constexpr int kStallMs = 30000;
constexpr qint64 kEveryHours = 6;
constexpr qint64 kMaxTree = 16LL << 20;
constexpr qint64 kMaxSha = 1024;
constexpr qint64 kStaleStagingSecs = 3600;
struct Limits {
    qint64 file = 50LL << 20;
    qint64 total = 200LL << 20;
    int entries = 5000;
};
Limits g_limits;

QString base() {
    return QString(qEnvironmentVariable("FUGUE_TEMPLATE_FEED")).remove(QRegularExpression(QStringLiteral("/+$")));
}

QUrl shaUrl() {
    return QUrl(base().isEmpty() ? QStringLiteral("https://api.github.com/repos/bmaltais/fugue-workflows/commits/main")
                                 : base() + QStringLiteral("/commits/main"));
}

QUrl treeUrl(const QString &sha) {
    return QUrl(base().isEmpty() ? QStringLiteral("https://api.github.com/repos/bmaltais/fugue-workflows/git/trees/") + sha
                                   + QStringLiteral("?recursive=1")
                                 : base() + QStringLiteral("/git/trees/") + sha + QStringLiteral("?recursive=1"));
}

QUrl rawUrl(const QString &sha, const QString &path) {
    QStringList parts;
    for (const QString &part : path.split(QLatin1Char('/')))
        parts << QString::fromLatin1(QUrl::toPercentEncoding(part));
    return QUrl((base().isEmpty() ? QStringLiteral("https://raw.githubusercontent.com/bmaltais/fugue-workflows")
                                  : base() + QStringLiteral("/raw"))
                + QLatin1Char('/') + sha + QLatin1Char('/') + parts.join(QLatin1Char('/')));
}

// https, or http to this machine for a test's stand-in server.
bool allowed(const QUrl &url) {
    if (url.scheme() == QLatin1String("https"))
        return true;
    const QString host = url.host();
    return url.scheme() == QLatin1String("http")
           && (host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1"));
}

bool isSha(const QString &text) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{40}$"));
    return pattern.match(text).hasMatch();
}

// A name inside the folder, as unpackZip takes them: relative, no "..", "\\" or ":" (and no empty part).
bool safeName(const QString &name) {
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.contains(QLatin1Char(':'))
        || name.contains(QLatin1Char('\0')))
        return false;
    for (const QString &part : name.split(QLatin1Char('/')))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    return true;
}

// What unpackZip leaves out: dot files and folders (.github, .gitignore) and __MACOSX.
bool leftOut(const QString &name) {
    for (const QString &part : name.split(QLatin1Char('/')))
        if (part.startsWith(QLatin1Char('.')) || part == QLatin1String("__MACOSX"))
            return true;
    return false;
}

// git's object id of a file: SHA-1 of "blob <size>\0" and the bytes.
QString blobSha(QIODevice &device, qint64 size) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QByteArray("blob " + QByteArray::number(size) + '\0'));
    hash.addData(&device);
    return QString::fromLatin1(hash.result().toHex());
}

QString blobShaOfFile(const QString &path, qint64 size) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != size)
        return {};
    return blobSha(file, size);
}

// The Default session and a chosen project share one store; a scratch FUGUE_HOME or --home root has its own.
bool shared() { return fugueHome().isEmpty() || rootIsChosenProject(); }
QString stateFolder() { return shared() ? baseStateDir() : stateDir(); }
QString statusPath() { return stateFolder() + QStringLiteral("/template-feed.json"); }
QString currentPath() { return TemplateFeed::storeDir() + QStringLiteral("/current"); }

void writeStatus(const QString &error, const QString &sha) {
    QDir().mkpath(stateFolder());
    QSaveFile file(statusPath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject{{"checked", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                         {"error", error},
                                         {"sha", sha}}).toJson());
    file.commit();
}

bool holdsTemplates(const QString &root) {
    for (const QFileInfo &entry : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo(entry.absoluteFilePath() + QStringLiteral("/template.json")).isFile())
            return true;
    return false;
}

// A hard link (from the previous fetched copy, which Fugue itself made) when it can be, else a copy. Never over a file.
bool linkOrCopy(const QString &from, const QString &to, bool mayLink) {
    if (mayLink) {
        std::error_code ec;
        std::filesystem::create_hard_link(from.toStdString(), to.toStdString(), ec);
        if (!ec)
            return true;
    }
    return QFile::copy(from, to); // QFile::copy never replaces a file
}

} // namespace

TemplateFeed::TemplateFeed(QNetworkAccessManager *network, QObject *parent) : QObject(parent), m_network(network) {}

bool TemplateFeed::disabledByEnvironment() { return !qEnvironmentVariable("FUGUE_TEMPLATES").isEmpty(); }

QString TemplateFeed::storeDir() { return (shared() ? baseDataDir() : dataDir()) + QStringLiteral("/templates-online"); }

bool TemplateFeed::storeIsShared() { return shared(); }

bool TemplateFeed::automaticAllowed() {
    return !disabledByEnvironment() && shared() && !TemplateLibrary::bundledIsBuildTree();
}

void TemplateFeed::setLimitsForTest(qint64 maxFile, qint64 maxTotal, int maxEntries) { g_limits = {maxFile, maxTotal, maxEntries}; }
void TemplateFeed::resetLimitsForTest() { g_limits = {}; }

TemplateFeed::~TemplateFeed() = default;

QString TemplateFeed::currentSha() {
    QFile file(currentPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString sha = QString::fromLatin1(file.readAll()).trimmed();
    return isSha(sha) ? sha : QString();
}

QString TemplateFeed::currentRoot() {
    const QString sha = currentSha();
    const QString folder = storeDir() + QLatin1Char('/') + sha;
    return !sha.isEmpty() && QFileInfo(folder).isDir() ? folder : QString();
}

FeedStatus TemplateFeed::status() {
    QFile file(statusPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    return {QDateTime::fromString(json.value("checked").toString(), Qt::ISODate), json.value("error").toString(),
            json.value("sha").toString()};
}

bool TemplateFeed::due(const QDateTime &now) {
    const QDateTime checked = status().checked;
    return !checked.isValid() || checked > now || checked.secsTo(now) >= kEveryHours * 3600;
}

void TemplateFeed::get(const QUrl &url, const QByteArray &accept, void (TemplateFeed::*then)(), qint64 limit, const QString &overLimit) {
    if (!allowed(url))
        return fail(QStringLiteral("The template address %1 is not https.").arg(url.toString()));
    QNetworkRequest request(url);
    // Redirects stay on the same host and port: GitHub's own (a renamed repo) work, and a test's loopback server cannot
    // send Fugue elsewhere.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(kStallMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("Fugue"));
    if (!accept.isEmpty())
        request.setRawHeader("Accept", accept);
    m_overLimit.clear();
    m_reply = m_network->get(request);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply, limit, overLimit](qint64 got, qint64 total) {
        if (got > limit || total > limit) {
            m_overLimit = overLimit;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, then);
}

void TemplateFeed::check() {
    if (m_busy)
        return;
    m_busy = true;
    m_wanted.clear();
    QDir().mkpath(storeDir());
    m_lock = std::make_unique<QLockFile>(storeDir() + QStringLiteral("/.lock"));
    m_lock->setStaleLockTime(0); // only a holder that died is stale: a live update may take long
    if (!m_lock->tryLock(0)) {
        m_lock.reset();
        m_previous = currentSha();
        return done({FeedResult::Outcome::Busy, m_previous, m_previous, QStringLiteral("Another Fugue is updating the templates."), 0, 0});
    }
    m_previous = currentSha(); // read under the lock: another Fugue may have just switched
    // Staging folders a crashed update left (this process's own name is replaced when it stages).
    const QDateTime old = QDateTime::currentDateTimeUtc().addSecs(-kStaleStagingSecs);
    for (const QFileInfo &stale : QDir(storeDir()).entryInfoList({QStringLiteral(".staging-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        if (stale.lastModified().toUTC() < old)
            QDir(stale.absoluteFilePath()).removeRecursively();
    get(shaUrl(), "application/vnd.github.sha", &TemplateFeed::gotSha, kMaxSha, QStringLiteral("GitHub's answer is too long for a commit."));
}

// The body of the reply, or empty with `why` set.
QByteArray TemplateFeed::answer(QNetworkReply *reply, QString *why) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!m_overLimit.isEmpty()) {
        *why = m_overLimit;
    } else if (status == 429 || (status == 403 && reply->rawHeader("x-ratelimit-remaining") == "0")) {
        *why = QStringLiteral("GitHub's rate limit is reached; try again later.");
    } else if (status == 403) {
        *why = QStringLiteral("GitHub refused the request (403).");
    } else if (reply->error() != QNetworkReply::NoError) {
        *why = QStringLiteral("GitHub could not be reached: %1.").arg(reply->errorString());
    }
    return why->isEmpty() ? reply->readAll() : QByteArray();
}

void TemplateFeed::gotSha() {
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    QString why;
    const QString sha = QString::fromLatin1(answer(reply, &why)).trimmed();
    if (!why.isEmpty())
        return fail(why);
    if (!isSha(sha))
        return fail(QStringLiteral("GitHub's answer is not a commit."));
    if (sha == m_previous && !currentRoot().isEmpty())
        return done({FeedResult::Outcome::Current, sha, m_previous, {}, 0, 0});
    m_sha = sha;
    get(treeUrl(sha), "application/vnd.github+json", &TemplateFeed::gotTree, kMaxTree, QStringLiteral("GitHub's template list is too long."));
}

void TemplateFeed::gotTree() {
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    QString why;
    const QByteArray bytes = answer(reply, &why);
    if (!why.isEmpty())
        return fail(why);
    const QJsonObject tree = QJsonDocument::fromJson(bytes).object();
    if (tree.value("truncated").toBool())
        return fail(QStringLiteral("The template list is too long for GitHub to give whole; keeping what is here."));
    if (!tree.value("tree").isArray())
        return fail(QStringLiteral("GitHub's template list is not readable."));

    QList<Item> items;
    QSet<QString> names;
    qint64 total = 0;
    for (const QJsonValue value : tree.value("tree").toArray()) {
        const QJsonObject entry = value.toObject();
        const QString type = entry.value("type").toString();
        const QString mode = entry.value("mode").toString();
        const QString path = entry.value("path").toString();
        if (type == QLatin1String("tree"))
            continue;
        if (!safeName(path))
            return fail(QStringLiteral("The template list names %1, outside its folder.").arg(path));
        if (leftOut(path))
            continue; // dot files and folders, whatever they are (a link in .github too)
        if (type != QLatin1String("blob") || (mode != QLatin1String("100644") && mode != QLatin1String("100755")))
            return fail(QStringLiteral("The template list has %1, which is not a plain file.").arg(path));
        const QJsonValue sizeValue = entry.value("size");
        const qint64 size = sizeValue.isDouble() ? sizeValue.toInteger(-1) : -1;
        const QString sha = entry.value("sha").toString();
        if (size < 0 || !isSha(sha) || names.contains(path))
            return fail(QStringLiteral("The template list is not readable at %1.").arg(path));
        if (size > g_limits.file)
            return fail(QStringLiteral("%1 is over %2 MB.").arg(path).arg(qMax<qint64>(1, g_limits.file >> 20)));
        total += size;
        if (total > g_limits.total || items.size() >= g_limits.entries)
            return fail(QStringLiteral("The templates are over the size Fugue takes."));
        names.insert(path);
        items.append({path, sha, size, mode == QLatin1String("100755")});
    }
    if (items.isEmpty())
        return fail(QStringLiteral("The template list is empty."));
    // A name that is a file and also a folder holding files cannot be a folder of templates.
    for (const Item &item : items)
        for (qsizetype slash = item.path.indexOf(QLatin1Char('/')); slash > 0; slash = item.path.indexOf(QLatin1Char('/'), slash + 1))
            if (names.contains(item.path.left(slash)))
                return fail(QStringLiteral("The template list has %1 as a file and as a folder.").arg(item.path.left(slash)));

    // Stage the new copy beside the store: what the base holds unchanged is linked or copied, the rest is wanted.
    m_staging = storeDir() + QStringLiteral("/.staging-") + QString::number(::getpid());
    QDir(m_staging).removeRecursively();
    if (!QDir().mkpath(m_staging))
        return fail(QStringLiteral("Fugue could not make %1.").arg(m_staging));
    const QString previous = currentRoot();
    const QString from = !previous.isEmpty() ? previous : (m_bundled.isEmpty() ? TemplateLibrary::bundledRoot() : m_bundled);
    const bool mayLink = !previous.isEmpty(); // only from a copy Fugue made itself, never the bundled files
    m_files = int(items.size());
    m_downloaded = 0;
    for (const Item &item : items) {
        const QString source = from.isEmpty() ? QString() : from + QLatin1Char('/') + item.path;
        const QString target = m_staging + QLatin1Char('/') + item.path;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return fail(QStringLiteral("Fugue could not make a folder for %1.").arg(item.path));
        if (!source.isEmpty() && QFileInfo(source).isFile() && !QFileInfo(source).isSymLink()
            && blobShaOfFile(source, item.size) == item.sha) {
            if (!linkOrCopy(source, target, mayLink))
                return fail(QStringLiteral("Fugue could not copy %1.").arg(item.path));
        } else {
            m_wanted.append(item);
        }
    }
    nextDownload();
}

void TemplateFeed::nextDownload() {
    if (m_wanted.isEmpty())
        return finish();
    const Item &item = m_wanted.constFirst();
    get(rawUrl(m_sha, item.path), {}, &TemplateFeed::gotFile, item.size,
        QStringLiteral("%1 is larger than the template list says.").arg(item.path));
}

void TemplateFeed::gotFile() {
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    const Item item = m_wanted.takeFirst();
    QString why;
    const QByteArray bytes = answer(reply, &why);
    if (!why.isEmpty())
        return fail(why);
    const QByteArray head = "blob " + QByteArray::number(bytes.size()) + '\0';
    if (bytes.size() != item.size
        || QString::fromLatin1(QCryptographicHash::hash(head + bytes, QCryptographicHash::Sha1).toHex()) != item.sha)
        return fail(QStringLiteral("%1 is not the file the template list names; it was refused.").arg(item.path));
    QFile file(m_staging + QLatin1Char('/') + item.path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size())
        return fail(QStringLiteral("Fugue could not write %1: %2.").arg(item.path, file.errorString()));
    if (item.executable)
        file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    file.close();
    ++m_downloaded;
    nextDownload();
}

// Names the staged copy into place, switches `current`, and drops the folders older than the previous one.
void TemplateFeed::finish() {
    if (!holdsTemplates(m_staging))
        return fail(QStringLiteral("The templates hold no templates."));
    // Which templates the bundled root held now: a snapshot hides only the bundled ids that were there and are not in it.
    QJsonArray bundledIds;
    const QString bundledRoot = m_bundled.isEmpty() ? TemplateLibrary::bundledRoot() : m_bundled;
    if (!bundledRoot.isEmpty())
        for (const QFileInfo &entry : QDir(bundledRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (QFileInfo(entry.absoluteFilePath() + QStringLiteral("/template.json")).isFile()
                || QFileInfo(entry.absoluteFilePath() + QStringLiteral("/workflow.fugue")).isFile())
                bundledIds.append(entry.fileName());
    QFile idsFile(m_staging + QStringLiteral("/.bundled-ids.json"));
    if (!idsFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) || idsFile.write(QJsonDocument(QJsonObject{{"bundled", bundledIds}}).toJson()) < 0)
        return fail(QStringLiteral("Fugue could not write the template record."));
    idsFile.close();
    const QString target = storeDir() + QLatin1Char('/') + m_sha;
    // Never remove a live folder in place: other Fugues may be reading it. It is renamed aside, then removed.
    QString aside;
    if (QFileInfo::exists(target)) {
        aside = storeDir() + QStringLiteral("/.staging-old-") + m_sha + QLatin1Char('-') + QString::number(::getpid()); // the staging sweep removes it if this process dies
        QDir(aside).removeRecursively();
        if (!QDir().rename(target, aside))
            return fail(QStringLiteral("Fugue could not replace %1.").arg(target));
    }
    if (!QDir().rename(m_staging, target))
        return fail(QStringLiteral("Fugue could not move the templates into %1.").arg(target));
    QSaveFile current(currentPath());
    if (!current.open(QIODevice::WriteOnly) || current.write(m_sha.toLatin1() + '\n') < 0 || !current.commit()) {
        QDir(target).removeRecursively();
        return fail(QStringLiteral("Fugue could not write %1.").arg(currentPath()));
    }
    if (!aside.isEmpty())
        QDir(aside).removeRecursively();
    // The previous one stays: a window that scanned it a moment ago may still be reading it.
    for (const QFileInfo &old : QDir(storeDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        if (old.fileName() != m_sha && old.fileName() != m_previous && isSha(old.fileName()))
            QDir(old.absoluteFilePath()).removeRecursively();
    done({FeedResult::Outcome::Updated, m_sha, m_previous, {}, m_downloaded, m_files});
}

void TemplateFeed::fail(const QString &why) {
    if (!m_staging.isEmpty())
        QDir(m_staging).removeRecursively();
    m_staging.clear();
    m_wanted.clear();
    done({FeedResult::Outcome::Failed, m_previous, m_previous, why, 0, 0});
}

void TemplateFeed::done(FeedResult result) {
    m_busy = false;
    m_staging.clear();
    m_lock.reset();
    if (result.outcome == FeedResult::Outcome::Failed)
        qWarning().noquote() << "templates: not updated:" << result.error;
    if (result.outcome != FeedResult::Outcome::Busy)
        writeStatus(result.error, result.sha);
    emit finished(result);
}

FeedResult TemplateFeed::checkNow() {
    QNetworkAccessManager network;
    TemplateFeed feed(&network);
    FeedResult result;
    QEventLoop loop;
    connect(&feed, &TemplateFeed::finished, &loop, [&](const FeedResult &done) {
        result = done;
        loop.quit();
    });
    feed.check();
    if (feed.busy())
        loop.exec();
    return result;
}

} // namespace fugue
