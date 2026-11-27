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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <filesystem>
#include <unistd.h>

#include "engine/paths.h"
#include "engine/templates.h"

namespace fugue {

namespace {

constexpr int kStallMs = 30000;
constexpr qint64 kEveryHours = 6;
constexpr qint64 kMaxFile = 50LL << 20;
constexpr qint64 kMaxTotal = 200LL << 20;
constexpr int kMaxEntries = 5000;

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

QString statusPath() { return stateDir() + QStringLiteral("/template-feed.json"); }
QString currentPath() { return TemplateFeed::storeDir() + QStringLiteral("/current"); }

void writeStatus(const QString &error, const QString &sha) {
    QDir().mkpath(stateDir());
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

// A hard link when the base is on the same filesystem, else a copy.
bool linkOrCopy(const QString &from, const QString &to) {
    std::error_code ec;
    std::filesystem::create_hard_link(from.toStdString(), to.toStdString(), ec);
    return !ec || QFile::copy(from, to);
}

} // namespace

TemplateFeed::TemplateFeed(QNetworkAccessManager *network, QObject *parent) : QObject(parent), m_network(network) {}

bool TemplateFeed::disabledByEnvironment() { return !qEnvironmentVariable("FUGUE_TEMPLATES").isEmpty(); }

QString TemplateFeed::storeDir() { return dataDir() + QStringLiteral("/templates-online"); }

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

void TemplateFeed::get(const QUrl &url, const QByteArray &accept, void (TemplateFeed::*then)()) {
    if (!allowed(url))
        return fail(QStringLiteral("The template address %1 is not https.").arg(url.toString()));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kStallMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("Fugue"));
    if (!accept.isEmpty())
        request.setRawHeader("Accept", accept);
    m_reply = m_network->get(request);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 got, qint64 total) {
        if (got > kMaxFile || total > kMaxFile)
            reply->abort(); // nothing here is bigger than a file's cap
    });
    connect(reply, &QNetworkReply::finished, this, then);
}

void TemplateFeed::check() {
    if (m_busy)
        return;
    m_busy = true;
    m_previous = currentSha();
    m_wanted.clear();
    get(shaUrl(), "application/vnd.github.sha", &TemplateFeed::gotSha);
}

// The reply of the step that just ran, or null after a failure was reported.
static QByteArray answer(QNetworkReply *reply, QString *why) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 403 || status == 429)
        *why = QStringLiteral("GitHub's rate limit is reached; try again later.");
    else if (reply->error() != QNetworkReply::NoError)
        *why = QStringLiteral("GitHub could not be reached: %1.").arg(reply->errorString());
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
    get(treeUrl(sha), "application/vnd.github+json", &TemplateFeed::gotTree);
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
        if (type != QLatin1String("blob") || (mode != QLatin1String("100644") && mode != QLatin1String("100755")))
            return fail(QStringLiteral("The template list has %1, which is not a plain file.").arg(path));
        if (leftOut(path))
            continue;
        const qint64 size = qint64(entry.value("size").toDouble(-1));
        const QString sha = entry.value("sha").toString();
        if (size < 0 || !isSha(sha) || names.contains(path))
            return fail(QStringLiteral("The template list is not readable at %1.").arg(path));
        if (size > kMaxFile)
            return fail(QStringLiteral("%1 is over %2 MB.").arg(path).arg(kMaxFile >> 20));
        total += size;
        if (total > kMaxTotal || items.size() >= kMaxEntries)
            return fail(QStringLiteral("The templates are over the size Fugue takes."));
        names.insert(path);
        items.append({path, sha, size, mode == QLatin1String("100755")});
    }
    if (items.isEmpty())
        return fail(QStringLiteral("The template list is empty."));

    // Stage the new copy beside the store: what the base holds unchanged is linked or copied, the rest is wanted.
    const QString store = storeDir();
    m_staging = store + QStringLiteral("/.staging-") + QString::number(::getpid());
    QDir(m_staging).removeRecursively();
    if (!QDir().mkpath(m_staging))
        return fail(QStringLiteral("Fugue could not make %1.").arg(m_staging));
    const QString from = !currentRoot().isEmpty() ? currentRoot() : (m_bundled.isEmpty() ? TemplateLibrary::bundledRoot() : m_bundled);
    m_files = int(items.size());
    m_downloaded = 0;
    for (const Item &item : items) {
        const QString source = from.isEmpty() ? QString() : from + QLatin1Char('/') + item.path;
        const QString target = m_staging + QLatin1Char('/') + item.path;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return fail(QStringLiteral("Fugue could not make a folder for %1.").arg(item.path));
        if (!source.isEmpty() && QFileInfo(source).isFile() && !QFileInfo(source).isSymLink()
            && blobShaOfFile(source, item.size) == item.sha) {
            if (!linkOrCopy(source, target))
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
    get(rawUrl(m_sha, m_wanted.constFirst().path), {}, &TemplateFeed::gotFile);
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
    QByteArray head = "blob " + QByteArray::number(bytes.size()) + '\0';
    if (bytes.size() != item.size
        || QString::fromLatin1(QCryptographicHash::hash(head + bytes, QCryptographicHash::Sha1).toHex()) != item.sha)
        return fail(QStringLiteral("%1 is not the file the template list names; it was refused.").arg(item.path));
    QFile file(m_staging + QLatin1Char('/') + item.path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return fail(QStringLiteral("Fugue could not write %1: %2.").arg(item.path, file.errorString()));
    if (item.executable)
        file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    file.close();
    ++m_downloaded;
    nextDownload();
}

// Names the staged copy into place, switches `current` and drops the older folders.
void TemplateFeed::finish() {
    if (!holdsTemplates(m_staging))
        return fail(QStringLiteral("The templates hold no templates."));
    const QString target = storeDir() + QLatin1Char('/') + m_sha;
    if (QFileInfo::exists(target) && !QDir(target).removeRecursively())
        return fail(QStringLiteral("Fugue could not replace %1.").arg(target));
    if (!QDir().rename(m_staging, target))
        return fail(QStringLiteral("Fugue could not move the templates into %1.").arg(target));
    QSaveFile current(currentPath());
    if (!current.open(QIODevice::WriteOnly) || current.write(m_sha.toLatin1() + '\n') < 0 || !current.commit()) {
        QDir(target).removeRecursively();
        return fail(QStringLiteral("Fugue could not write %1.").arg(currentPath()));
    }
    for (const QFileInfo &old : QDir(storeDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (old.fileName() != m_sha && isSha(old.fileName()))
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
    if (result.outcome == FeedResult::Outcome::Failed)
        qWarning().noquote() << "templates: not updated:" << result.error;
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
