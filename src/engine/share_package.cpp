#include "share_package.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>

#include <memory>

#include "engine/atomic_file.h"
#include "engine/document.h"
#include "engine/file_inputs.h"
#include "engine/file_roots.h"
#include "engine/group_library.h"
#include "engine/image_format.h"
#include "engine/json.h"
#include "engine/registry.h"
#include "engine/zip_archive.h"

namespace fugue {

namespace {

constexpr qint64 kMaxPacked = qint64(3) << 30;   // every input of one package, written
constexpr qint64 kMaxUnpacked = qint64(4) << 30; // a whole package, unpacked
constexpr int kMaxEntries = 10000;

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

// Points every `file` input of `document` at `inputs/<name>` and lists the file to put there.
// Only an image Fugue takes in (image_format.h: by its bytes, at most maxImageBytes) goes in, so
// what is shared is what the person sees on the canvas, never another file a node names;
// another file, or one not there, is a warning, its input keeping the bare file name.
Errors bundleInputs(Document &document, const Registry &registry, const QString &workflowPath, const QString &id,
                    QList<ZipEntry> &entries, QStringList *included) {
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
        const std::optional<ImageFormat> format = sniffInputFile(file.read(128));
        if (!format || info.size() > maxImageBytes) {
            warn(node, input,
                 format ? QStringLiteral("%1 is over %2 MB, so it is not in the package.").arg(bare).arg(maxImageBytes >> 20)
                        : QStringLiteral("%1 is not an image or a paint file, so it is not in the package.").arg(bare));
            value = InputValue::literal(bare);
            return;
        }
        const QString name = uniqueName(packedName(path, *format), path, claimed);
        const QString inside = id + QStringLiteral("/inputs/") + name;
        bool listed = false;
        for (const ZipEntry &entry : std::as_const(entries))
            listed = listed || entry.name == inside;
        if (!listed) {
            total += info.size();
            entries << ZipEntry{inside, {}, path};
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

} // namespace

Errors writeSharePackage(const Document &document, const Registry &registry, const ShareRequest &request,
                         const QString &zipPath, QStringList *included) {
    const QString title = request.title.trimmed().isEmpty() ? QStringLiteral("Workflow") : request.title.trimmed();
    const QString id = folderId(title);
    QList<ZipEntry> entries;
    Document packed = document;
    // A library or pack group it names only by its type travels as its definition: a group is
    // a graph of other nodes, no program, so the person needs neither the library nor the pack.
    embedGroupTypes(packed, registry);
    Errors errors = bundleInputs(packed, registry, request.workflowPath, id, entries, included);
    if (hasErrors(errors))
        return errors;
    packed.refreshRequirements(request.appVersion, &registry);
    entries.prepend(ZipEntry{id + QStringLiteral("/template.json"), writeJson(QJsonObject{{"title", title}}), {}});
    entries.prepend(ZipEntry{id + QStringLiteral("/workflow.fugue"),
                          writeJson(shareCopy(packed.toJson(), registry, request.options)), {}});
    if (!request.preview.isEmpty()) {
        QFile preview(request.preview);
        const std::optional<ImageFormat> format =
            preview.open(QIODevice::ReadOnly) ? sniffImage(preview.read(16)) : std::nullopt;
        if (format && QStringList{"png", "webp", "jpg"}.contains(format->extension) && preview.size() <= maxImageBytes)
            entries << ZipEntry{id + QStringLiteral("/preview.") + format->extension, {}, request.preview};
    }

    if (const Errors written = writeZipFile(entries, zipPath); hasErrors(written))
        return written;
    return errors; // the warnings
}

bool isSharePackage(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(4) == QByteArray("PK\x03\x04", 4);
}

Result<QString> unpackSharePackage(const QString &zipPath, const QString &folder) {
    const Errors unpacked = unpackZip(zipPath, folder, UnpackLimits{maxImageBytes, kMaxUnpacked, kMaxEntries});
    if (hasErrors(unpacked))
        return Result<QString>(unpacked);

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
