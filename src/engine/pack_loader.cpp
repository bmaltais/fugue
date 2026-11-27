#include "engine/pack_loader.h"
#include "engine/command_node.h"

#include "engine/group_library.h"
#include "engine/json.h"
#include "engine/pack_manager.h"
#include "engine/pack_secrets.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "engine/settings.h"
#include "engine/trust_store.h"
#include "engine/version.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <cstdio>

namespace fugue {

namespace {

// A pack's git URL as a workflow may carry it to others (#91): an https, ssh or git URL with no
// password (and, for https, no user), or git's user@host:path form; none for a local one.
QString shareableSource(const PackSource &source) {
    if (source.kind != PackSource::Kind::Git || source.location.isEmpty())
        return {};
    static const QRegularExpression scp(QStringLiteral("^[A-Za-z0-9._-]+@[A-Za-z0-9.-]+:[^ ]+$"));
    if (scp.match(source.location).hasMatch())
        return source.location;
    QUrl url(source.location, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()
        || !QStringList{"https", "ssh", "git"}.contains(url.scheme()))
        return {};
    url.setPassword({});
    if (url.scheme() == QLatin1String("https"))
        url.setUserName({});
    return url.toString();
}


const QStringList knownPackFields = {"manifest", "id",    "version", "name",  "license",   "source",
                                     "description", "nodes", "types", "setup", "converters", "settings"};

Error problem(const QString &code, const QString &message, Error::Severity severity = Error::Severity::Error) {
    Error error = makeError(code, message);
    error.severity = severity;
    return error;
}

Error invalid(const QString &message) { return problem(QStringLiteral("invalid_document"), message); }

bool isPackId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$"));
    return pattern.match(id).hasMatch();
}

// A node folder: its name, or `<name>@<major>` for an older major kept beside the newest (3.6).
bool isFolderName(const QString &name) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9-]*(@(0|[1-9][0-9]{0,8}))?$"));
    return pattern.match(name).hasMatch();
}

// A model folder name a resource type declares: one path segment, not hidden (a folder starting
// with "." is never searched), as ComfyUI's are ("upscale_models", "ESRGAN").
bool isModelFolderName(const QString &name) {
    return !name.isEmpty() && !name.startsWith(QLatin1Char('.')) && !name.contains(QLatin1Char('/'))
           && !name.contains(QLatin1Char('\\'));
}

// A path inside the node's own folder: relative, and never leaving it.
bool isInsideFolder(const QString &path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.startsWith(QLatin1Char('~')))
        return false;
    const QString clean = QDir::cleanPath(path);
    return clean != QLatin1String("..") && !clean.startsWith(QLatin1String("../")) && clean != QLatin1String(".");
}

bool isPositiveNumber(const QJsonValue &value) { return value.isDouble() && value.toDouble() > 0; }

struct PackManifest {
    QString id;
    QString version;
    QString name;
    QStringList nodes;
    QList<DeclaredPortType> types;
    QList<PackSecretSpec> secrets;
};

// Validates pack.json, reporting every problem found.
std::optional<PackManifest> parsePack(const QJsonObject &json, PackOrigin origin, Errors &errors) {
    const qsizetype before = errors.size();
    PackManifest pack;

    if (!json.value("manifest").isDouble())
        errors << invalid(QStringLiteral("pack.json needs \"manifest\": 1."));
    else if (json.value("manifest").toInt() != 1)
        errors << problem(QStringLiteral("unsupported_format_version"),
                          QStringLiteral("pack.json has manifest format %1; this Fugue reads format 1.")
                              .arg(json.value("manifest").toDouble()));

    pack.id = json.value("id").toString();
    const bool reserved = pack.id == QLatin1String("fugue") || pack.id == QLatin1String("local");
    if (origin == PackOrigin::Library) {
        // The group library has the one reserved id it may use.
        if (pack.id != QLatin1String("local"))
            errors << invalid(QStringLiteral("The group library's pack.json must have the id local, not \"%1\".").arg(pack.id));
    } else if (origin == PackOrigin::Disk && (reserved || pack.id.startsWith(QLatin1String("fugue.")) ||
                                       pack.id.startsWith(QLatin1String("local."))))
        errors << invalid(QStringLiteral("The pack id \"%1\" belongs to Fugue; a pack folder cannot use it.").arg(pack.id));
    else if (!(isPackId(pack.id) || (reserved && origin == PackOrigin::BuiltIn)))
        errors << invalid(QStringLiteral("pack.json \"id\" must be lowercase reverse-DNS, like io.github.alice.tools."));

    pack.version = json.value("version").toString();
    if (!Version::parse(pack.version))
        errors << invalid(QStringLiteral("pack.json \"version\" must be MAJOR.MINOR.PATCH."));

    pack.name = json.value("name").toString();
    if (pack.name.trimmed().isEmpty())
        errors << invalid(QStringLiteral("pack.json needs a \"name\"."));

    for (const char *key : {"license", "source", "description"})
        if (json.contains(key) && !json.value(key).isString())
            errors << invalid(QStringLiteral("pack.json \"%1\" must be text.").arg(key));

    if (!json.value("nodes").isArray()) {
        errors << invalid(QStringLiteral("pack.json needs \"nodes\", a list of node folder names."));
    } else {
        for (const QJsonValue &entry : json.value("nodes").toArray()) {
            const QString name = entry.toString();
            if (!isFolderName(name))
                errors << invalid(QStringLiteral("pack.json \"nodes\" must list plain folder names, not %1.")
                                      .arg(entry.isString() ? QStringLiteral("\"%1\"").arg(name) : QStringLiteral("that")));
            else if (pack.nodes.contains(name))
                errors << invalid(QStringLiteral("pack.json lists the node folder \"%1\" twice.").arg(name));
            else
                pack.nodes << name;
        }
    }

    if (json.contains("types")) {
        if (!json.value("types").isArray()) {
            errors << invalid(QStringLiteral("pack.json \"types\" must be a list."));
        } else {
            for (const QJsonValue &entry : json.value("types").toArray()) {
                const QJsonObject type = entry.toObject();
                const QString id = type.value("id").toString();
                const QJsonValue versionValue = type.value("version");
                const int version = versionValue.toInt(0);
                const QString kind = type.value("kind").toString();
                DeclaredPortType declared;
                const auto parsed = PortType::parse(version > 1 ? QStringLiteral("%1@%2").arg(id).arg(version) : id);
                const QJsonValue extensions = type.value("extensions");
                bool extensionsOk = extensions.isUndefined() || extensions.isArray();
                for (const QJsonValue &extension : extensions.toArray())
                    extensionsOk = extensionsOk && extension.isString() && !extension.toString().isEmpty();
                // Model folder names under each models root (8.9): one plain name each, as
                // ComfyUI's ("text_encoders"), and only on a resource type.
                const QJsonValue folders = type.value("folders");
                bool foldersOk = folders.isUndefined() || (folders.isArray() && kind == QLatin1String("resource"));
                for (const QJsonValue &folder : folders.toArray())
                    foldersOk = foldersOk && isModelFolderName(folder.toString());
                // What its files are, for a model folder of one kind (8.9, #86).
                const QJsonValue files = type.value("files");
                static const QStringList kinds{"diffusion-model", "text-encoder", "vae", "lora", "checkpoint"};
                foldersOk = foldersOk && (files.isUndefined() || (kind == QLatin1String("resource") && kinds.contains(files.toString())));
                if (!entry.isObject() || !extensionsOk || !foldersOk || id.contains(QLatin1Char('@')) || !parsed || parsed->isBuiltin() || parsed->isAny() ||
                    !id.startsWith(pack.id + QLatin1Char('/')) || !versionValue.isDouble() || version < 1 ||
                    (kind != QLatin1String("resource") && kind != QLatin1String("data")) ||
                    type.value("name").toString().isEmpty()) {
                    errors << invalid(QStringLiteral("pack.json \"types\" entry %1 needs an id %2/<name>, a version "
                                                     "of 1 or more, a kind of \"resource\" or \"data\", a name and, if any, "
                                                     "extensions as a list of text and (for a resource) folders as a list of "
                                                     "plain folder names and files as one of diffusion-model, "
                                                     "text-encoder, vae, lora or checkpoint.")
                                          .arg(id.isEmpty() ? QStringLiteral("(no id)") : QStringLiteral("\"%1\"").arg(id), pack.id));
                    continue;
                }
                declared.type = *parsed;
                declared.kind = kind == QLatin1String("resource") ? PortKind::Resource : PortKind::Data;
                declared.name = type.value("name").toString();
                for (const QJsonValue &extension : type.value("extensions").toArray())
                    declared.extensions << extension.toString();
                for (const QJsonValue &folder : folders.toArray())
                    declared.folders << folder.toString();
                declared.files = files.toString();
                pack.types << declared;
            }
        }
    }

    if (json.contains("setup")) {
        const QJsonObject setup = json.value("setup").toObject();
        const QJsonArray command = setup.value("command").toArray();
        bool ok = json.value("setup").isObject() && !command.isEmpty();
        for (const QJsonValue &word : command)
            ok = ok && word.isString() && !word.toString().isEmpty();
        if (!ok)
            errors << invalid(QStringLiteral("pack.json \"setup.command\" must be a program and its arguments, "
                                             "like [\"uv\", \"sync\"]."));
    }

    if (json.contains("settings")) {
        const Result<QList<PackSecretSpec>> secrets = parsePackSecrets(json.value("settings"));
        if (secrets)
            pack.secrets = *secrets;
        else
            errors << secrets.errors();
    }

    for (auto it = json.begin(); it != json.end(); ++it)
        if (!knownPackFields.contains(it.key()))
            errors << problem(QStringLiteral("invalid_document"),
                              QStringLiteral("pack.json has an unknown field \"%1\"; it is ignored.").arg(it.key()),
                              Error::Severity::Warning);

    if (hasErrors(errors.mid(before)))
        return std::nullopt;
    return pack;
}

// What is wrong with a node's `runtime` (6.1, 6.9), without opening anything it names;
// empty when it is fine or absent (the registry reports an absent one).
QString runtimeProblem(const QJsonObject &runtime, PackOrigin origin) {
    if (runtime.isEmpty())
        return {};
    const QString kind = runtime.value("kind").toString();
    if (origin == PackOrigin::Library && kind != QLatin1String("group"))
        return QStringLiteral("The group library holds groups only; runtime.kind \"%1\" is not one.").arg(kind);
    const auto path = [&](const char *key) -> QString {
        const QString value = runtime.value(key).toString();
        return isInsideFolder(value) ? QString()
                                     : QStringLiteral("runtime.%1 must be a path inside the node's folder.").arg(key);
    };
    if (kind == QLatin1String("command")) {
        if (const QString bad = path("entry"); !bad.isEmpty())
            return bad;
        const QJsonValue mode = runtime.value("mode");
        if (!mode.isUndefined() && mode != QLatin1String("oneshot") && mode != QLatin1String("persistent"))
            return QStringLiteral("runtime.mode must be \"oneshot\" or \"persistent\".");
        for (const char *key : {"idle_timeout_s", "timeout_s"})
            if (runtime.contains(key) && !isPositiveNumber(runtime.value(key)))
                return QStringLiteral("runtime.%1 must be a number above 0.").arg(key);
        return {};
    }
    if (kind == QLatin1String("group"))
        return path("definition");
    if (kind == QLatin1String("native")) {
        if (const QString bad = path("entry"); !bad.isEmpty())
            return bad;
        static const QRegularExpression abiPattern(QStringLiteral("^(0|[1-9][0-9]{0,3})\\.(0|[1-9][0-9]{0,3})$"));
        const auto match = abiPattern.match(runtime.value("abi").toString());
        if (!match.hasMatch())
            return QStringLiteral("runtime.abi must be \"<major>.<min_host_minor>\", like \"1.0\".");
        if (match.captured(1).toInt() != kHostAbiMajor || match.captured(2).toInt() > kHostAbiMinor)
            return QStringLiteral("runtime.abi %1 needs a newer host than this Fugue (ABI %2.%3).")
                .arg(runtime.value("abi").toString())
                .arg(kHostAbiMajor)
                .arg(kHostAbiMinor);
        return {};
    }
    if (kind == QLatin1String("builtin"))
        return origin == PackOrigin::BuiltIn ? QString()
                                             : QStringLiteral("runtime.kind \"builtin\" is only for the fugue pack.");
    if (kind == QLatin1String("script"))
        return QStringLiteral("runtime.kind \"script\" is reserved and not available in this version.");
    return QStringLiteral("runtime.kind \"%1\" is not known; use command, group or native.").arg(kind);
}

// The one sentence a person sees for a node that did not load.
QString reasonOf(const Errors &errors) {
    QString reason;
    qsizetype count = 0;
    for (const Error &error : errors) {
        if (error.severity != Error::Severity::Error)
            continue;
        if (count++ == 0)
            reason = error.message;
    }
    if (count > 1)
        reason += QStringLiteral(" (and %1 more)").arg(count - 1);
    return reason;
}

// Loads one node folder; empty reason on success, the type id in `typeId`.
QString loadNode(Registry &registry, const PackManifest &pack, const QString &folder, const QString &name,
                 PackOrigin origin, QString &typeId, Errors &warnings) {
    const QString path = folder + QLatin1Char('/') + name + QStringLiteral("/node.json");
    const Result<QJsonObject> read = readJsonObject(path);
    if (!read.ok())
        return read.error().code == QLatin1String("file_not_found")
                   ? QStringLiteral("%1/node.json is missing.").arg(name)
                   : reasonOf(read.errors());
    if (QFileInfo(folder + QLatin1Char('/') + name).isSymLink())
        return QStringLiteral("The node folder \"%1\" is a symbolic link; put the folder itself in the pack.").arg(name);
    const QJsonObject &manifest = read.value();
    // An alias is an old id of this very node: one in another pack's namespace (or fugue/ or
    // local/) would make a workflow of that pack, or a built-in, resolve to this node.
    for (const QJsonValue &alias : manifest.value("aliases").toArray())
        if (!alias.toString().startsWith(pack.id + QLatin1Char('/')))
            return QStringLiteral("The alias \"%1\" must start with \"%2/\": a node can only be known by old ids of its own pack.")
                .arg(alias.toString(), pack.id);
    const QString id = manifest.value("id").toString();
    const PackRef nodeRef = parsePackRef(name); // `<name>@<major>`: an older major beside the newest
    if (id != pack.id + QLatin1Char('/') + nodeRef.id)
        return QStringLiteral("The node id \"%1\" must be \"%2/%3\": the pack's id, then the folder name.")
            .arg(id, pack.id, nodeRef.id);
    if (nodeRef.major >= 0) {
        const std::optional<Version> version = Version::parse(manifest.value("version").toString());
        if (!version || version->major != nodeRef.major)
            return QStringLiteral("The folder \"%1\" holds major version %2, so node.json's version must be %2.x.y.")
                .arg(name)
                .arg(nodeRef.major);
    }
    if (const QString bad = runtimeProblem(manifest.value("runtime").toObject(), origin); !bad.isEmpty())
        return bad;
    const QJsonObject runtime = manifest.value("runtime").toObject();
    const QString nodeDir = folder + QLatin1Char('/') + name;
    const Result<NodeType> parsed = NodeType::fromJson(manifest);
    Errors errors;
    if (parsed) {
        NodeType type = *parsed;
        type.dir = nodeDir;
        if (origin != PackOrigin::BuiltIn)
            type.packVersion = pack.version; // the built-ins' versions are the app's, keyed by the node's own
        type.packSecrets = packSecretNames(pack.secrets);
        for (const PackSecretSpec &spec : pack.secrets)
            if (spec.originRequired)
                type.packSecretsOriginRequired << spec.name;
        if (!pack.secrets.isEmpty())
            type.packSource = readPackSource(folder).location;
        if (runtime.value("kind").toString() == QLatin1String("command"))
            type.seal = nodeSeal(nodeDir, runtime.value("entry").toString());
        errors = registry.add(type);
    } else {
        errors = parsed.errors();
    }
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Warning) {
            Error warning = error;
            warning.message = QStringLiteral("%1: %2").arg(name, error.message);
            warnings << warning;
        }
    if (hasErrors(errors))
        return reasonOf(errors);
    typeId = id;
    return {};
}

} // namespace

QString packFolderName(const QString &id, int major) { return QStringLiteral("%1@%2").arg(id).arg(major); }

PackRef parsePackRef(const QString &folderName) {
    static const QRegularExpression suffix(QStringLiteral("^(.+)@(0|[1-9][0-9]{0,8})$"));
    const QRegularExpressionMatch match = suffix.match(folderName);
    if (!match.hasMatch())
        return {folderName, -1};
    return {match.captured(1), match.captured(2).toInt()};
}

QString packsDir() { return dataDir() + QStringLiteral("/packs"); }

QStringList packSearchPaths() {
    return searchPaths(packsDir(), currentSettings().packPaths);
}

QStringList inheritedPackPaths() {
    if (!sessionInherits().contains(QStringLiteral("packs")))
        return {};
    return searchPaths(baseDataDir() + QStringLiteral("/packs"), readSettings(baseSettingsPath()).settings.packPaths);
}

namespace {

struct PackIdentity {
    QString id;
    QString version;
    int major = -1;
};

// The id, version and major of the pack.json in `folder`, or an empty id.
PackIdentity identityOf(const QString &folder) {
    PackIdentity identity;
    const Result<QJsonObject> read = readJsonObject(folder + QStringLiteral("/pack.json"));
    if (!read)
        return identity;
    const std::optional<Version> version = Version::parse(read->value("version").toString());
    if (!version || read->value("id").toString().isEmpty())
        return identity;
    identity.id = read->value("id").toString();
    identity.version = read->value("version").toString();
    identity.major = version->major;
    return identity;
}

// The id and major of every pack in the folders of `paths`.
QSet<QString> packKeysIn(const QStringList &paths) {
    QSet<QString> keys;
    for (const QString &path : paths)
        for (const QFileInfo &entry : QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (!entry.fileName().startsWith(QLatin1Char('.')))
                if (const PackIdentity identity = identityOf(entry.filePath()); !identity.id.isEmpty())
                    keys.insert(QStringLiteral("%1@%2").arg(identity.id).arg(identity.major));
    return keys;
}

} // namespace

QString packSecretsDigest(const QString &id, const QString &version) {
    QStringList paths = packSearchPaths();
    paths << inheritedPackPaths();
    for (const QString &path : paths)
        for (const QFileInfo &entry : QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (entry.fileName().startsWith(QLatin1Char('.')) || parsePackRef(entry.fileName()).id != id)
                continue;
            const Result<QJsonObject> read = readJsonObject(entry.filePath() + QStringLiteral("/pack.json"));
            if (!read || read->value("id").toString() != id || read->value("version").toString() != version)
                continue;
            // The name and the origin rule of each declared secret: what decides which values a pack is handed
            // and where they may go. Labels and descriptions are only text.
            QStringList lines;
            for (const QJsonValue &setting : read->value("settings").toArray())
                lines << setting.toObject().value("name").toString() + QLatin1Char(':') + setting.toObject().value("origin").toString();
            lines.sort();
            return QString::fromLatin1(QCryptographicHash::hash(lines.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha256).toHex());
        }
    return {};
}

QList<InheritedPack> inheritedPacks() {
    QList<InheritedPack> found;
    const QStringList paths = inheritedPackPaths();
    if (paths.isEmpty())
        return found;
    const QSet<QString> own = packKeysIn(packSearchPaths());
    QSet<QString> seen;
    for (const QString &path : paths)
        for (const QFileInfo &entry : QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (entry.fileName().startsWith(QLatin1Char('.')) || entry.fileName() == QLatin1String("local"))
                continue;
            const PackIdentity identity = identityOf(entry.filePath());
            const QString key = QStringLiteral("%1@%2").arg(identity.id).arg(identity.major);
            if (identity.id.isEmpty() || TrustStore::isBuiltIn(identity.id) || own.contains(key) || seen.contains(key))
                continue;
            seen.insert(key);
            found.append({identity.id, identity.version, entry.filePath(), identity.major});
        }
    return found;
}

bool isInheritedPack(const QString &id, const QString &version) {
    for (const InheritedPack &pack : inheritedPacks())
        if (pack.id == id && pack.version == version)
            return true;
    return false;
}

QString inheritedPackFolder(const QString &folderName) {
    const PackRef ref = parsePackRef(folderName);
    for (const InheritedPack &pack : inheritedPacks())
        if (pack.id == ref.id && (ref.major < 0 || pack.major == ref.major) && QFileInfo(pack.folder).fileName() == folderName)
            return pack.folder;
    return {};
}

QString inheritedPackWarning() {
    return QStringLiteral("Inherited from Default: this pack runs from the Default project's folder, so anything it writes (its setup "
                          "environment, caches, downloads, state) lands in the Default project's folders, not this project's. "
                          "Fugue cannot stop that.");
}

bool PackReport::hasProblems() const {
    for (const LoadedPack &pack : packs)
        if (pack.hidden)
            continue;
        else if (!pack.enabled || !pack.disabled.isEmpty() || hasErrors(pack.errors))
            return true;
    return false;
}

LoadedPack loadPack(Registry &registry, const QString &folder, PackOrigin origin) {
    LoadedPack loaded;
    loaded.path = folder;

    const Result<QJsonObject> read = readJsonObject(folder + QStringLiteral("/pack.json"));
    if (!read.ok()) {
        loaded.errors = read.errors();
        return loaded;
    }
    const std::optional<PackManifest> pack = parsePack(read.value(), origin, loaded.errors);
    loaded.id = read.value().value("id").toString();
    loaded.version = read.value().value("version").toString();
    loaded.name = read.value().value("name").toString();
    if (pack)
        loaded.secrets = pack->secrets;
    if (!pack)
        return loaded;
    const int major = Version::parse(pack->version)->major; // parsePack checked it
    if (origin != PackOrigin::BuiltIn) {
        const QString name = QFileInfo(folder).fileName();
        const PackRef ref = parsePackRef(name);
        if (name != pack->id && !(ref.id == pack->id && ref.major == major)) {
            loaded.errors << invalid(QStringLiteral("The folder is named \"%1\" but pack.json says the id is %2 at version %3; a pack "
                                                    "lives in a folder named %2, or %2@%4 beside another major.")
                                         .arg(name, pack->id, pack->version)
                                         .arg(major));
            return loaded;
        }
    }
    if (registry.hasPackMajor(pack->id, major)) {
        loaded.errors << invalid(QStringLiteral("A pack with the id %1 is already loaded at major version %2 (%3); this one is skipped.")
                                     .arg(pack->id)
                                     .arg(major)
                                     .arg(registry.packVersions(pack->id).value(0)));
        return loaded;
    }

    // A type another pack already declared keeps its owner; ours is reported and the nodes
    // using it will say why they cannot load.
    for (const DeclaredPortType &type : pack->types)
        loaded.errors << registry.add(type);

    for (const QString &name : pack->nodes) {
        QString typeId;
        const QString reason = loadNode(registry, *pack, folder, name, origin, typeId, loaded.errors);
        if (!reason.isEmpty())
            loaded.disabled << PackProblem{name, reason};
        else if (!loaded.nodes.contains(typeId))
            loaded.nodes << typeId; // an older major of a node lists once
    }
    loaded.nodes.sort();

    for (const QFileInfo &entry : QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (!pack->nodes.contains(entry.fileName()) && QFileInfo::exists(entry.filePath() + QStringLiteral("/node.json")))
            loaded.errors << problem(QStringLiteral("invalid_document"),
                                     QStringLiteral("The folder \"%1\" has a node.json but pack.json does not list it; "
                                                    "it is not loaded.").arg(entry.fileName()),
                                     Error::Severity::Warning);

    registry.setPackVersion(pack->id, pack->version);
    if (const QString shared = shareableSource(readPackSource(folder)); !shared.isEmpty()) {
        const PackSource source = readPackSource(folder);
        registry.setPackSource(pack->id, pack->version, shared, source.commit.isEmpty() ? source.ref : source.commit);
    }
    loaded.enabled = true;
    return loaded;
}

PackReport loadPacks(Registry &registry, const QStringList &searchPaths) { return loadPacks(registry, searchPaths, {}); }

PackReport loadPacks(Registry &registry, const QStringList &searchPaths, const QStringList &inherited) {
    PackReport report;
    QSet<QString> seen;
    const qsizetype own = searchPaths.size();
    const QStringList all = searchPaths + inherited;
    for (qsizetype i = 0; i < all.size(); ++i) {
        const QDir dir(all[i]);
        if (!dir.exists() || seen.contains(dir.canonicalPath()))
            continue;
        seen.insert(dir.canonicalPath());
        const bool fromDefault = i >= own;
        for (const QFileInfo &entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (entry.fileName().startsWith(QLatin1Char('.')))
                continue;
            if (fromDefault) {
                // The Default's group library is not shared, and an own pack of the same id and major hides the
                // Default's quietly: the project's copy is the one it chose.
                if (entry.fileName() == QLatin1String("local"))
                    continue;
                const PackIdentity identity = identityOf(entry.filePath());
                if (!identity.id.isEmpty() && registry.hasPackMajor(identity.id, identity.major)) {
                    LoadedPack hidden;
                    hidden.path = entry.filePath();
                    hidden.id = identity.id;
                    hidden.version = identity.version;
                    hidden.inherited = true;
                    hidden.hidden = true;
                    report.packs << hidden;
                    continue;
                }
                LoadedPack loaded = loadPack(registry, entry.filePath(), PackOrigin::Disk);
                loaded.inherited = true;
                report.packs << loaded;
                continue;
            }
            const bool library = i == 0 && entry.fileName() == QLatin1String("local") && !entry.isSymLink();
            if (library)
                healLibrary(entry.filePath());
            report.packs << loadPack(registry, entry.filePath(), library ? PackOrigin::Library : PackOrigin::Disk);
        }
    }
    return report;
}

PackReport loadInstalledPacks(Registry &registry) {
    const PackReport report = loadPacks(registry, packSearchPaths(), inheritedPackPaths());
    for (const LoadedPack &pack : report.packs) {
        if (pack.hidden)
            continue;
        for (const Error &error : pack.errors)
            std::fprintf(stderr, "pack %s: %s: %s\n", qPrintable(pack.path), qPrintable(error.code), qPrintable(error.message));
        for (const PackProblem &node : pack.disabled)
            std::fprintf(stderr, "pack %s: node %s is disabled: %s\n", qPrintable(pack.path), qPrintable(node.node),
                         qPrintable(node.reason));
    }
    return report;
}

} // namespace fugue
