#include "share_package.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <archive.h>
#include <archive_entry.h>

#include <memory>

#include "engine/atomic_file.h"
#include "engine/document.h"
#include "engine/file_roots.h"
#include "engine/json.h"
#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue {

namespace {

constexpr qint64 kMaxInput = qint64(1) << 30;       // one input file
constexpr qint64 kMaxUnpacked = qint64(4) << 30;    // a whole package, unpacked
constexpr int kMaxEntries = 10000;
constexpr qint64 kChunk = 1 << 20;

Error fileError(const QString &code, const QString &message, const QString &hint = {}) {
    return makeError(code, message, hint);
}

Error notAPackage(const QString &zip, const QString &why) {
    return makeError(QStringLiteral("invalid_document"),
                     QStringLiteral("%1 is not a Fugue package: %2.").arg(QFileInfo(zip).fileName(), why),
                     QStringLiteral("Ask for the package again, exported with File > Export for sharing."));
}

// The template's folder name: the title in lowercase letters, digits and dashes.
QString folderId(const QString &title) {
    static const QRegularExpression other(QStringLiteral("[^a-z0-9]+"));
    QString id = title.toLower().replace(other, QStringLiteral("-"));
    while (id.startsWith(QLatin1Char('-')))
        id.remove(0, 1);
    while (id.endsWith(QLatin1Char('-')))
        id.chop(1);
    return id.isEmpty() ? QStringLiteral("workflow") : id.left(64);
}

// The name `source` takes in the package's inputs: its own, else `<stem>-2.<ext>`... `claimed`
// maps the names given so far to their sources (canonical), so a file named twice is copied once.
QString inputName(const QString &source, QHash<QString, QString> &claimed) {
    const QFileInfo info(source);
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? info.fileName()
                                    : QStringLiteral("%1-%2%3").arg(info.completeBaseName()).arg(n).arg(suffix);
        const auto taken = claimed.constFind(name);
        if (taken == claimed.constEnd()) {
            claimed.insert(name, source);
            return name;
        }
        if (*taken == source)
            return name;
    }
}

Errors copyFile(const QString &from, const QString &to) {
    if (!QFile::copy(from, to))
        return {fileError(QStringLiteral("permission_denied"),
                          QStringLiteral("Fugue could not copy %1 into the package.").arg(QFileInfo(from).fileName()))};
    return {};
}

// Copies every file a `file` input of `document` names into `inputs`, and points the input at
// `inputs/<name>`. A file that is not there is a warning; its input is left for the share copy.
Errors bundleInputs(Document &document, const Registry &registry, const QString &workflowPath, const QString &inputs) {
    Errors errors;
    QHash<QString, QString> claimed;
    const auto scan = [&](Nodes &nodes) {
        for (auto node = nodes.begin(); node != nodes.end(); ++node) {
            const NodeType *type = registry.nodeType(node->type);
            if (!type)
                continue;
            for (auto in = node->inputs.begin(); in != node->inputs.end(); ++in) {
                const InputSpec *spec = type->input(in.key());
                if (!spec || spec->widget() != QLatin1String("file") || in->kind() != InputValue::Kind::Literal
                    || !in->literal().isString() || in->literal().toString().trimmed().isEmpty())
                    continue;
                const Result<QString> located = inputFilePath(in->literal().toString(), workflowPath, in.key());
                const QString path = located ? canonicalPath(located.value()) : QString();
                const QFileInfo info(path);
                if (path.isEmpty() || !info.isFile()) {
                    Error warning = fileError(QStringLiteral("file_not_found"),
                                              QStringLiteral("%1 is not there, so it is not in the package.")
                                                  .arg(QFileInfo(in->literal().toString()).fileName()),
                                              QStringLiteral("Pick the file again, then export again."));
                    warning.severity = Error::Severity::Warning;
                    warning.node = node.key();
                    warning.input = in.key();
                    errors << warning;
                    continue;
                }
                if (info.size() > kMaxInput) {
                    errors << fileError(QStringLiteral("permission_denied"),
                                        QStringLiteral("%1 is over 1 GB, too large to share in a package.").arg(info.fileName()),
                                        QStringLiteral("Share that file another way, and export without it."));
                    continue;
                }
                const QString name = inputName(path, claimed);
                const QString target = inputs + QLatin1Char('/') + name;
                if (!QFileInfo::exists(target)) {
                    QDir().mkpath(inputs);
                    errors << copyFile(path, target);
                }
                *in = InputValue::literal(QStringLiteral("inputs/") + name);
            }
        }
    };
    scan(document.graph.nodes);
    for (auto group = document.groups.begin(); group != document.groups.end(); ++group)
        scan(group->nodes);
    return errors;
}

// Zips every file under `root` as `<prefix>/<its path under root>` into `zip`.
Errors zipFolder(const QString &root, const QString &prefix, const QString &zip) {
    std::unique_ptr<archive, decltype(&archive_write_free)> out(archive_write_new(), archive_write_free);
    const auto failed = [&](const QString &what) {
        return Errors{fileError(QStringLiteral("permission_denied"),
                                QStringLiteral("Fugue could not write the package %1: %2 (%3).")
                                    .arg(QFileInfo(zip).fileName(), what,
                                         QString::fromLocal8Bit(archive_error_string(out.get()))))};
    };
    if (archive_write_set_format_zip(out.get()) != ARCHIVE_OK
        || archive_write_open_filename(out.get(), QFile::encodeName(zip).constData()) != ARCHIVE_OK)
        return failed(QStringLiteral("open"));
    QStringList files;
    QDirIterator walk(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (walk.hasNext())
        files << walk.next();
    files.sort(); // the same package for the same files
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QString &file : std::as_const(files)) {
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly))
            return failed(in.errorString());
        const QByteArray name = (prefix + QLatin1Char('/') + QDir(root).relativeFilePath(file)).toUtf8();
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname_utf8(entry.get(), name.constData());
        archive_entry_set_size(entry.get(), in.size());
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_mtime(entry.get(), now, 0);
        if (archive_write_header(out.get(), entry.get()) != ARCHIVE_OK)
            return failed(QString::fromUtf8(name));
        while (!in.atEnd()) {
            const QByteArray chunk = in.read(kChunk);
            if (chunk.isEmpty() || archive_write_data(out.get(), chunk.constData(), size_t(chunk.size())) < 0)
                return failed(QString::fromUtf8(name));
        }
    }
    if (archive_write_close(out.get()) != ARCHIVE_OK)
        return failed(QStringLiteral("close"));
    return {};
}

// An entry's name is taken when it is relative and stays inside: no "..", no empty part, no "\".
bool safeName(const QString &name) {
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QLatin1Char(':')))
        return false;
    QStringList parts = name.split(QLatin1Char('/'));
    if (parts.last().isEmpty())
        parts.removeLast(); // a folder's trailing slash
    for (const QString &part : std::as_const(parts))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    return true;
}

// What a zip from another system adds and no template holds: macOS's resource forks, dot files.
bool leftOut(const QString &name) {
    for (const QString &part : name.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        if (part.startsWith(QLatin1Char('.')) || part == QLatin1String("__MACOSX"))
            return true;
    return false;
}

} // namespace

Errors writeSharePackage(const Document &document, const Registry &registry, const ShareRequest &request,
                         const QString &zipPath) {
    if (QFileInfo::exists(zipPath) || QFileInfo(zipPath).isSymLink())
        return {fileError(QStringLiteral("permission_denied"),
                          QStringLiteral("%1 is there already; Fugue does not replace it.").arg(zipPath),
                          QStringLiteral("Pick another name.")) };
    QTemporaryDir staging;
    if (!staging.isValid())
        return {fileError(QStringLiteral("permission_denied"),
                          QStringLiteral("Fugue could not make a folder to pack in: %1.").arg(staging.errorString()))};
    const QString id = folderId(request.title);
    const QString folder = staging.filePath(id);
    QDir().mkpath(folder);

    Document packed = document;
    Errors errors = bundleInputs(packed, registry, request.workflowPath, folder + QStringLiteral("/inputs"));
    if (hasErrors(errors))
        return errors;
    packed.refreshRequirements(request.appVersion, &registry);
    const QJsonObject shared = shareCopy(packed.toJson(), registry, request.options);
    for (const auto &[name, value] :
         {std::pair{QStringLiteral("workflow.fugue"), QJsonValue(shared)},
          std::pair{QStringLiteral("template.json"),
                    QJsonValue(QJsonObject{{"title", request.title.trimmed().isEmpty() ? id : request.title.trimmed()}})}})
        if (const Errors written = writeJsonFile(folder + QLatin1Char('/') + name, value); hasErrors(written))
            return written;
    if (!request.preview.isEmpty()) {
        QString suffix = QFileInfo(request.preview).suffix().toLower();
        if (suffix == QLatin1String("jpeg"))
            suffix = QStringLiteral("jpg");
        if (QStringList{"png", "webp", "jpg"}.contains(suffix) && QFileInfo(request.preview).isFile())
            if (const Errors copied = copyFile(request.preview, folder + QStringLiteral("/preview.") + suffix);
                hasErrors(copied))
                return copied;
    }

    // Written beside the target, then named into place without replacing anything there.
    const QFileInfo target(zipPath);
    PartialFile partial(target.absolutePath(), target.fileName());
    if (const Errors zipped = zipFolder(folder, id, partial.path()); hasErrors(zipped))
        return zipped;
    const Result<bool> committed = partial.commit(target.absoluteFilePath(), Overwrite::Never);
    if (!committed)
        return committed.errors();
    if (!*committed)
        return {fileError(QStringLiteral("permission_denied"),
                          QStringLiteral("A file came to %1 while the package was written; it was left as it is.").arg(zipPath),
                          QStringLiteral("Pick another name."))};
    return errors; // the warnings
}

bool isSharePackage(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(4) == QByteArray("PK\x03\x04", 4);
}

Result<QString> unpackSharePackage(const QString &zipPath, const QString &folder) {
    std::unique_ptr<archive, decltype(&archive_read_free)> in(archive_read_new(), archive_read_free);
    archive_read_support_format_zip(in.get());
    if (archive_read_open_filename(in.get(), QFile::encodeName(zipPath).constData(), 64 * 1024) != ARCHIVE_OK)
        return notAPackage(zipPath, QString::fromLocal8Bit(archive_error_string(in.get())));
    archive_entry *entry = nullptr;
    int entries = 0;
    qint64 unpacked = 0;
    int read = ARCHIVE_OK;
    while ((read = archive_read_next_header(in.get(), &entry)) == ARCHIVE_OK) {
        const char *utf8 = archive_entry_pathname_utf8(entry);
        const QString name = QString::fromUtf8(utf8 ? utf8 : archive_entry_pathname(entry));
        if (!safeName(name))
            return notAPackage(zipPath, QStringLiteral("it names %1, outside its folder").arg(name));
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
            return notAPackage(zipPath, QStringLiteral("%1 is a link").arg(name));
        if (++entries > kMaxEntries)
            return notAPackage(zipPath, QStringLiteral("it holds more than %1 files").arg(kMaxEntries));
        if (leftOut(name))
            continue;
        const QString path = folder + QLatin1Char('/') + name;
        const mode_t type = archive_entry_filetype(entry);
        if (type == AE_IFDIR) {
            QDir().mkpath(path);
            continue;
        }
        if (type != AE_IFREG)
            return notAPackage(zipPath, QStringLiteral("%1 is not a file").arg(name));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path);
        if (out.exists() || !out.open(QIODevice::WriteOnly))
            return notAPackage(zipPath, QStringLiteral("%1 cannot be unpacked").arg(name));
        QByteArray buffer(kChunk, Qt::Uninitialized);
        for (;;) {
            const la_ssize_t got = archive_read_data(in.get(), buffer.data(), size_t(buffer.size()));
            if (got < 0)
                return notAPackage(zipPath, QString::fromLocal8Bit(archive_error_string(in.get())));
            if (got == 0)
                break;
            unpacked += got;
            if (unpacked > kMaxUnpacked)
                return notAPackage(zipPath, QStringLiteral("it unpacks to more than 4 GB"));
            if (out.write(buffer.constData(), got) != got)
                return makeError(QStringLiteral("permission_denied"),
                                 QStringLiteral("Fugue could not unpack %1: %2.").arg(name, out.errorString()));
        }
    }
    if (read != ARCHIVE_EOF)
        return notAPackage(zipPath, QString::fromLocal8Bit(archive_error_string(in.get())));

    // Its files at its top: they go in a folder named after the zip, as a template's would be.
    if (QFileInfo::exists(folder + QStringLiteral("/workflow.fugue"))) {
        const QString inner = folder + QLatin1Char('/') + folderId(QFileInfo(zipPath).completeBaseName());
        QDir().mkpath(inner);
        for (const QFileInfo &item : QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
            if (item.absoluteFilePath() != inner && !QDir().rename(item.absoluteFilePath(), inner + QLatin1Char('/') + item.fileName()))
                return notAPackage(zipPath, QStringLiteral("%1 cannot be unpacked").arg(item.fileName()));
        return inner;
    }
    const QFileInfoList tops = QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (tops.size() != 1 || !QFileInfo::exists(tops.first().absoluteFilePath() + QStringLiteral("/workflow.fugue")))
        return notAPackage(zipPath, QStringLiteral("it holds no workflow.fugue in one folder"));
    return tops.first().absoluteFilePath();
}

} // namespace fugue
