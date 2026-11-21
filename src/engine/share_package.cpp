#include "share_package.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>

#include <archive.h>
#include <archive_entry.h>

#include <memory>

#include "engine/atomic_file.h"
#include "engine/document.h"
#include "engine/file_inputs.h"
#include "engine/file_roots.h"
#include "engine/group_library.h"
#include "engine/image_format.h"
#include "engine/json.h"
#include "engine/registry.h"

namespace fugue {

namespace {

constexpr qint64 kMaxPacked = qint64(3) << 30;   // every input of one package, written
constexpr qint64 kMaxUnpacked = qint64(4) << 30; // a whole package, unpacked
constexpr int kMaxEntries = 10000;
constexpr qint64 kChunk = 1 << 20;

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

// A file name every system unpacks and no template check skips: letters, digits, "._-" only,
// not starting with a dot; the image's own extension when the name has none.
QString packedName(const QString &source, const ImageFormat &format) {
    static const QRegularExpression other(QStringLiteral("[^A-Za-z0-9._-]"));
    QString name = QFileInfo(source).fileName().replace(other, QStringLiteral("_"));
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    if (QFileInfo(name).completeBaseName().isEmpty())
        name = QStringLiteral("image") + name;
    if (QFileInfo(name).suffix().isEmpty())
        name += QLatin1Char('.') + format.extension;
    return name.left(120);
}

// The name `source` takes among the package's inputs: its own, else `<stem>-2.<ext>`...
// `claimed` maps the names given so far to their sources, so a file named twice goes in once.
QString uniqueName(const QString &wanted, const QString &source, QHash<QString, QString> &claimed) {
    const QFileInfo info(wanted);
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? wanted : QStringLiteral("%1-%2%3").arg(info.completeBaseName()).arg(n).arg(suffix);
        const auto taken = claimed.constFind(name);
        if (taken == claimed.constEnd()) {
            claimed.insert(name, source);
            return name;
        }
        if (*taken == source)
            return name;
    }
}

// One file of the zip: its name in it, and its bytes or the file they are read from.
struct Entry {
    QString name;
    QByteArray bytes;
    QString file;
};

// Points every `file` input of `document` at `inputs/<name>` and lists the file to put there.
// Only an image Fugue takes in (image_format.h: by its bytes, at most maxImageBytes) goes in, so
// what is shared is what the person sees on the canvas, never another file a node names;
// another file, or one not there, is a warning, its input keeping the bare file name.
Errors bundleInputs(Document &document, const Registry &registry, const QString &workflowPath, const QString &id,
                    QList<Entry> &entries, QStringList *included) {
    Errors errors;
    QHash<QString, QString> claimed;
    qint64 total = 0;
    const auto warn = [&](const QString &node, const QString &input, const QString &message) {
        Error warning = makeError(QStringLiteral("file_not_found"), message,
                                  QStringLiteral("Pick the image again, then export again."));
        warning.severity = Error::Severity::Warning;
        warning.node = node;
        warning.input = input;
        errors << warning;
    };
    forEachFileLiteral(document, registry, [&](const QString &node, const QString &input, InputValue &value) {
        const QString text = value.literal().toString();
        if (text.trimmed().isEmpty())
            return;
        const QString bare = QFileInfo(text.trimmed()).fileName();
        const Result<QString> located = inputFilePath(text, workflowPath, input);
        const QString path = located ? canonicalPath(located.value()) : QString();
        const QFileInfo info(path);
        QFile file(path);
        if (path.isEmpty() || !info.isFile() || !file.open(QIODevice::ReadOnly)) {
            warn(node, input, QStringLiteral("%1 is not there, so it is not in the package.").arg(bare));
            value = InputValue::literal(bare);
            return;
        }
        const std::optional<ImageFormat> format = sniffImage(file.read(16));
        if (!format || info.size() > maxImageBytes) {
            warn(node, input,
                 format ? QStringLiteral("%1 is over %2 MB, so it is not in the package.").arg(bare).arg(maxImageBytes >> 20)
                        : QStringLiteral("%1 is not an image, so it is not in the package.").arg(bare));
            value = InputValue::literal(bare);
            return;
        }
        const QString name = uniqueName(packedName(path, *format), path, claimed);
        const QString inside = id + QStringLiteral("/inputs/") + name;
        bool listed = false;
        for (const Entry &entry : std::as_const(entries))
            listed = listed || entry.name == inside;
        if (!listed) {
            total += info.size();
            entries << Entry{inside, {}, path};
            if (included)
                *included << info.fileName();
        }
        value = InputValue::literal(QStringLiteral("inputs/") + name);
    });
    if (total > kMaxPacked)
        errors << makeError(QStringLiteral("value_out_of_range"),
                            QStringLiteral("Its images come to %1 GB; a package holds at most %2 GB.")
                                .arg(double(total) / (1 << 30), 0, 'f', 1)
                                .arg(kMaxPacked >> 30),
                            QStringLiteral("Share the largest images another way, and export without them."));
    return errors;
}

// Writes `entries` as the zip `zip`.
Errors writeZip(const QList<Entry> &entries, const QString &zip) {
    std::unique_ptr<archive, decltype(&archive_write_free)> out(archive_write_new(), archive_write_free);
    const auto failed = [&](const QString &what) {
        const int error = archive_errno(out.get());
        return Errors{makeError(writeFailureCode(error > 0 ? error : EIO),
                                QStringLiteral("Fugue could not write the package %1: %2 (%3).")
                                    .arg(QFileInfo(zip).fileName(), what,
                                         QString::fromLocal8Bit(archive_error_string(out.get()))))};
    };
    if (archive_write_set_format_zip(out.get()) != ARCHIVE_OK
        || archive_write_open_filename(out.get(), QFile::encodeName(zip).constData()) != ARCHIVE_OK)
        return failed(QStringLiteral("open"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const Entry &item : entries) {
        QFile in(item.file);
        if (!item.file.isEmpty() && !in.open(QIODevice::ReadOnly))
            return {makeError(QStringLiteral("file_not_found"),
                              QStringLiteral("Fugue could not read %1: %2.").arg(item.file, in.errorString()))};
        const QByteArray name = item.name.toUtf8();
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname_utf8(entry.get(), name.constData());
        archive_entry_set_size(entry.get(), item.file.isEmpty() ? item.bytes.size() : in.size());
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_mtime(entry.get(), now, 0);
        if (archive_write_header(out.get(), entry.get()) != ARCHIVE_OK)
            return failed(item.name);
        if (item.file.isEmpty()) {
            if (archive_write_data(out.get(), item.bytes.constData(), size_t(item.bytes.size())) < 0)
                return failed(item.name);
            continue;
        }
        while (!in.atEnd()) {
            const QByteArray chunk = in.read(kChunk);
            if (chunk.isEmpty() || archive_write_data(out.get(), chunk.constData(), size_t(chunk.size())) < 0)
                return failed(item.name);
        }
    }
    if (archive_write_close(out.get()) != ARCHIVE_OK)
        return failed(QStringLiteral("close"));
    return {};
}

// An entry's name is taken when it is relative and stays inside: no "..", ".", or empty part;
// no "\" or ":" (a Windows path, a drive), which no package Fugue writes holds.
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
                         const QString &zipPath, QStringList *included) {
    if (QFileInfo::exists(zipPath) || QFileInfo(zipPath).isSymLink())
        return {makeError(QStringLiteral("permission_denied"),
                          QStringLiteral("%1 is there already; Fugue does not replace it.").arg(zipPath),
                          QStringLiteral("Pick another name."))};
    const QString title = request.title.trimmed().isEmpty() ? QStringLiteral("Workflow") : request.title.trimmed();
    const QString id = folderId(title);
    QList<Entry> entries;
    Document packed = document;
    // A library or pack group it names only by its type travels as its definition: a group is
    // a graph of other nodes, no program, so the person needs neither the library nor the pack.
    embedGroupTypes(packed, registry);
    Errors errors = bundleInputs(packed, registry, request.workflowPath, id, entries, included);
    if (hasErrors(errors))
        return errors;
    packed.refreshRequirements(request.appVersion, &registry);
    entries.prepend(Entry{id + QStringLiteral("/template.json"), writeJson(QJsonObject{{"title", title}}), {}});
    entries.prepend(Entry{id + QStringLiteral("/workflow.fugue"),
                          writeJson(shareCopy(packed.toJson(), registry, request.options)), {}});
    if (!request.preview.isEmpty()) {
        QFile preview(request.preview);
        const std::optional<ImageFormat> format =
            preview.open(QIODevice::ReadOnly) ? sniffImage(preview.read(16)) : std::nullopt;
        if (format && QStringList{"png", "webp", "jpg"}.contains(format->extension) && preview.size() <= maxImageBytes)
            entries << Entry{id + QStringLiteral("/preview.") + format->extension, {}, request.preview};
    }

    // Written beside the target, then named into place without replacing anything there.
    const QFileInfo target(zipPath);
    PartialFile partial(target.absolutePath(), target.fileName());
    if (const Errors zipped = writeZip(entries, partial.path()); hasErrors(zipped))
        return zipped;
    const Result<bool> committed = partial.commit(target.absoluteFilePath(), Overwrite::Never);
    if (!committed)
        return committed.errors();
    if (!*committed)
        return {makeError(QStringLiteral("permission_denied"),
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
    QByteArray buffer(kChunk, Qt::Uninitialized);
    while ((read = archive_read_next_header(in.get(), &entry)) == ARCHIVE_OK) {
        const char *utf8 = archive_entry_pathname_utf8(entry);
        const char *raw = utf8 ? utf8 : archive_entry_pathname(entry);
        const QString name = QString::fromUtf8(raw ? raw : "");
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
        qint64 size = 0;
        for (;;) {
            const la_ssize_t got = archive_read_data(in.get(), buffer.data(), size_t(buffer.size()));
            if (got < 0)
                return notAPackage(zipPath, QString::fromLocal8Bit(archive_error_string(in.get())));
            if (got == 0)
                break;
            size += got;
            unpacked += got;
            // Counted as the bytes come, whatever the entry says of its size.
            if (size > maxImageBytes)
                return notAPackage(zipPath, QStringLiteral("%1 is over %2 MB").arg(name).arg(maxImageBytes >> 20));
            if (unpacked > kMaxUnpacked)
                return notAPackage(zipPath, QStringLiteral("it unpacks to more than 4 GB"));
            if (out.write(buffer.constData(), got) != got)
                return makeError(writeFailureCode(EIO),
                                 QStringLiteral("Fugue could not unpack %1: %2.").arg(name, out.errorString()));
        }
    }
    if (read != ARCHIVE_EOF)
        return notAPackage(zipPath, QString::fromLocal8Bit(archive_error_string(in.get())));

    // Its files at its top: they go in a new folder named after the zip, as a template's would be.
    if (QFileInfo::exists(folder + QStringLiteral("/workflow.fugue"))) {
        const QString base = folderId(QFileInfo(zipPath).completeBaseName());
        QString inner = folder + QLatin1Char('/') + base;
        for (int n = 2; QFileInfo::exists(inner); ++n)
            inner = folder + QLatin1Char('/') + base + QLatin1Char('-') + QString::number(n);
        const QFileInfoList items = QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        QDir().mkpath(inner);
        for (const QFileInfo &item : items)
            if (!QDir().rename(item.absoluteFilePath(), inner + QLatin1Char('/') + item.fileName()))
                return notAPackage(zipPath, QStringLiteral("%1 cannot be unpacked").arg(item.fileName()));
        return inner;
    }
    const QFileInfoList tops = QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (tops.size() != 1 || !QFileInfo::exists(tops.first().absoluteFilePath() + QStringLiteral("/workflow.fugue")))
        return notAPackage(zipPath, QStringLiteral("it holds no workflow.fugue in one folder"));
    return tops.first().absoluteFilePath();
}

} // namespace fugue
