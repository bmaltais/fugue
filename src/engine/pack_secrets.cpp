#include "engine/pack_secrets.h"

#include <algorithm>

#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include "engine/atomic_file.h"
#include "engine/json.h"
#include "engine/paths.h"

namespace fugue {

namespace {

Error invalid(const QString &message) {
    Error error;
    error.code = QStringLiteral("invalid_document");
    error.message = message;
    return error;
}

bool validName(const QString &name) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9_]{0,39}$"));
    return pattern.match(name).hasMatch();
}

} // namespace

Result<QList<PackSecretSpec>> parsePackSecrets(const QJsonValue &settings) {
    if (settings.isUndefined())
        return QList<PackSecretSpec>{};
    if (!settings.isArray())
        return invalid(QStringLiteral("pack.json \"settings\" must be a list of secrets, like "
                                      "[{\"name\": \"token_1\", \"type\": \"text\", \"secret\": true}]."));
    const QJsonArray list = settings.toArray();
    if (list.size() > 16)
        return invalid(QStringLiteral("pack.json \"settings\" declares %1 secrets; at most 16 are allowed.").arg(list.size()));
    QList<PackSecretSpec> specs;
    Errors errors;
    QStringList seen;
    for (const QJsonValue &entry : list) {
        const QJsonObject object = entry.toObject();
        const QString name = object.value("name").toString();
        const QString shown = name.isEmpty() ? QStringLiteral("(no name)") : QStringLiteral("\"%1\"").arg(name.left(40));
        if (!entry.isObject() || !validName(name)) {
            errors << invalid(QStringLiteral("pack.json \"settings\" entry %1 needs a \"name\" of lowercase letters, digits and "
                                             "_, starting with a letter (at most 40), like token_1.").arg(shown));
            continue;
        }
        if (object.value("secret") != true) {
            errors << invalid(QStringLiteral("pack.json setting %1 must have \"secret\": true; a pack's settings are secrets "
                                             "(a key a node needs), nothing else yet.").arg(shown));
            continue;
        }
        if (object.contains("type") && object.value("type") != QLatin1String("text")) {
            errors << invalid(QStringLiteral("pack.json setting %1 has a type other than \"text\".").arg(shown));
            continue;
        }
        bool textOk = true;
        for (const char *key : {"label", "description"})
            textOk = textOk && (!object.contains(key) || object.value(key).isString());
        if (!textOk) {
            errors << invalid(QStringLiteral("pack.json setting %1: \"label\" and \"description\" must be text.").arg(shown));
            continue;
        }
        const QJsonValue origin = object.value("origin");
        if (!origin.isUndefined() && origin != QLatin1String("required") && origin != QLatin1String("optional")) {
            errors << invalid(QStringLiteral("pack.json setting %1: \"origin\" must be \"required\" or \"optional\".").arg(shown));
            continue;
        }
        if (seen.contains(name)) {
            errors << invalid(QStringLiteral("pack.json declares the setting \"%1\" twice.").arg(name));
            continue;
        }
        seen << name;
        PackSecretSpec spec;
        spec.name = name;
        spec.label = object.value("label").toString(name);
        spec.description = object.value("description").toString();
        spec.originRequired = origin == QLatin1String("required");
        specs << spec;
    }
    if (!errors.isEmpty())
        return errors;
    return specs;
}

QStringList packSecretNames(const QList<PackSecretSpec> &specs) {
    QStringList names;
    for (const PackSecretSpec &spec : specs)
        names << spec.name;
    return names;
}

QString packSecretKey(const QString &packId, const QString &source) {
    return source.isEmpty() ? packId : packId + QLatin1Char('|') + source;
}

QString normalizeOrigin(const QString &text) {
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QLatin1String("https") && scheme != QLatin1String("http")) || url.host().isEmpty()
        || !url.userInfo().isEmpty() || (!url.path().isEmpty() && url.path() != QLatin1String("/")) || url.hasQuery() || url.hasFragment())
        return {};
    QString host = url.host().toLower();
    if (host.contains(QLatin1Char(':')))
        host = QStringLiteral("[%1]").arg(host);
    const int port = url.port(-1);
    const int standard = scheme == QLatin1String("https") ? 443 : 80;
    return scheme + QStringLiteral("://") + host + (port > 0 && port != standard ? QStringLiteral(":%1").arg(port) : QString());
}

Errors checkSecretValue(const QString &value) {
    Error error;
    error.code = QStringLiteral("usage");
    if (value.isEmpty())
        error.message = QStringLiteral("A secret cannot be empty; clear it instead.");
    else if (value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r')) || value.contains(QChar(0)))
        error.message = QStringLiteral("A secret is one line, with no line break or NUL.");
    else if (value.toUtf8().size() > 4096)
        error.message = QStringLiteral("A secret is at most 4096 bytes.");
    else
        return {};
    return {error};
}

namespace {

// Every way a value shows up in text: as it is, percent-encoded in either case of the hex digits (and with `+` for a
// space), and escaped inside a JSON string.
QStringList forms(const QString &value) {
    QStringList out{value};
    const QString upper = QString::fromLatin1(QUrl::toPercentEncoding(value));
    out << upper;
    QString lower = upper;
    static const QRegularExpression hex(QStringLiteral("%[0-9A-F]{2}"));
    for (auto it = hex.globalMatch(upper); it.hasNext();) {
        const auto match = it.next();
        lower.replace(match.capturedStart(), 3, match.captured().toLower());
    }
    out << lower;
    QString plus = upper;
    plus.replace(QStringLiteral("%20"), QStringLiteral("+"));
    out << plus;
    QString plusLower = lower;
    plusLower.replace(QStringLiteral("%20"), QStringLiteral("+"));
    out << plusLower;
    // JSON escapes: the string as JSON writes it between its quotes, with and without \u escapes of non-ASCII.
    const QByteArray json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    out << QString::fromUtf8(json.mid(2, json.size() - 4));
    out.removeDuplicates();
    return out;
}

QMutex &storeLock() {
    static QMutex mutex;
    return mutex;
}

} // namespace

QString maskSecrets(QString text, const QStringList &values) {
    // Longest form first, so a form that contains another is replaced whole.
    QStringList all;
    for (const QString &value : values)
        if (value.size() >= kMinMaskedSecretLength)
            all << forms(value);
    std::sort(all.begin(), all.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const QString &form : std::as_const(all))
        text.replace(form, QStringLiteral("***"));
    return text;
}

QString PackSecretStore::defaultPath() { return configDir() + QStringLiteral("/pack-secrets.json"); }

PackSecretStore::PackSecretStore(QString path) : m_path(std::move(path)) {}

Result<QJsonObject> PackSecretStore::read() const {
    if (!QFileInfo::exists(m_path))
        return QJsonObject{};
    const Result<QJsonObject> file = readJsonObject(m_path);
    if (!file)
        return file.errors();
    return file.value().value("packs").toObject();
}

namespace {

StoredSecret storedFrom(const QJsonValue &entry) {
    if (entry.isString()) // the first form of the file: a bare value, no origin
        return {entry.toString(), {}};
    const QJsonObject object = entry.toObject();
    return {object.value("value").toString(), object.value("origin").toString()};
}

} // namespace

QStringList PackSecretStore::namesSet(const QString &key) const {
    QMutexLocker lock(&storeLock());
    const Result<QJsonObject> packs = read();
    QStringList names;
    if (!packs)
        return names;
    const QJsonObject mine = packs->value(key).toObject();
    for (auto it = mine.begin(); it != mine.end(); ++it)
        if (!storedFrom(it.value()).value.isEmpty())
            names << it.key();
    names.sort();
    return names;
}

QString PackSecretStore::originOf(const QString &key, const QString &name) const {
    QMutexLocker lock(&storeLock());
    const Result<QJsonObject> packs = read();
    return packs ? storedFrom(packs->value(key).toObject().value(name)).origin : QString();
}

QStringList PackSecretStore::namesSetForOtherSources(const QString &packId, const QString &key) const {
    QMutexLocker lock(&storeLock());
    const Result<QJsonObject> packs = read();
    QStringList names;
    if (!packs)
        return names;
    for (auto pack = packs->begin(); pack != packs->end(); ++pack) {
        if (pack.key() == key || (pack.key() != packId && !pack.key().startsWith(packId + QLatin1Char('|'))))
            continue;
        const QJsonObject mine = pack.value().toObject();
        for (auto it = mine.begin(); it != mine.end(); ++it)
            if (!storedFrom(it.value()).value.isEmpty() && !names.contains(it.key()))
                names << it.key();
    }
    names.sort();
    return names;
}

Result<QMap<QString, StoredSecret>> PackSecretStore::values(const QString &key, const QStringList &declared) const {
    QMutexLocker lock(&storeLock());
    const Result<QJsonObject> packs = read();
    if (!packs)
        return packs.errors();
    const QJsonObject mine = packs->value(key).toObject();
    QMap<QString, StoredSecret> out;
    for (const QString &name : declared) {
        const StoredSecret secret = storedFrom(mine.value(name));
        if (!secret.value.isEmpty())
            out.insert(name, secret);
    }
    return out;
}

Errors PackSecretStore::write(const QJsonObject &packs) {
    return replaceFile(m_path, writeJson(QJsonObject{{"packs", packs}}), m_path, FileMode::OwnerOnly);
}

Errors PackSecretStore::set(const QString &key, const QString &name, const QString &value, const QString &origin,
                            bool originRequired) {
    if (const Errors bad = checkSecretValue(value); !bad.isEmpty())
        return bad;
    Error error;
    error.code = QStringLiteral("usage");
    if (!validName(name)) {
        error.message = QStringLiteral("\"%1\" is not a secret name.").arg(name.left(40));
        return {error};
    }
    QString where;
    if (!origin.isEmpty()) {
        where = normalizeOrigin(origin);
        if (where.isEmpty()) {
            error.message = QStringLiteral("\"%1\" is not an origin.").arg(origin.left(80));
            error.hint = QStringLiteral("Write it as https://api.example.com (a scheme, a host and maybe a port; no path).");
            return {error};
        }
    } else if (originRequired) {
        error.message = QStringLiteral("%1 must be set with the origin it is for.").arg(name);
        error.hint = QStringLiteral("Say where it may be sent, like https://api.example.com (--origin on the command line).");
        return {error};
    }
    QMutexLocker lock(&storeLock());
    Result<QJsonObject> packs = read();
    if (!packs)
        return packs.errors();
    QJsonObject mine = packs->value(key).toObject();
    QJsonObject entry{{"value", value}};
    if (!where.isEmpty())
        entry.insert("origin", where);
    mine.insert(name, entry);
    packs->insert(key, mine);
    return write(*packs);
}

Errors PackSecretStore::clear(const QString &key, const QString &name) {
    QMutexLocker lock(&storeLock());
    Result<QJsonObject> packs = read();
    if (!packs)
        return packs.errors();
    QJsonObject mine = packs->value(key).toObject();
    if (!mine.contains(name))
        return {};
    mine.remove(name);
    if (mine.isEmpty())
        packs->remove(key);
    else
        packs->insert(key, mine);
    return write(*packs);
}

Errors PackSecretStore::forgetPack(const QString &packId) {
    QMutexLocker lock(&storeLock());
    Result<QJsonObject> packs = read();
    if (!packs)
        return packs.errors();
    bool changed = false;
    for (const QString &key : packs->keys())
        if (key == packId || key.startsWith(packId + QLatin1Char('|'))) {
            packs->remove(key);
            changed = true;
        }
    return changed ? write(*packs) : Errors();
}

} // namespace fugue
