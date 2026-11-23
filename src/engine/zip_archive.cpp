#include "zip_archive.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <archive.h>
#include <archive_entry.h>

#include <memory>

#include "engine/atomic_file.h"

namespace fugue {

namespace {

constexpr qint64 kChunk = 1 << 20;

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

Error notAPackage(const QString &zip, const QString &why) {
    return makeError(QStringLiteral("invalid_document"),
                     QStringLiteral("%1 is not a Fugue package: %2.").arg(QFileInfo(zip).fileName(), why),
                     QStringLiteral("Ask for the package again, exported with File > Export for sharing."));
}


// Writes `entries` as the zip `zip`.
Errors writeZip(const QList<ZipEntry> &entries, const QString &zip) {
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
    for (const ZipEntry &item : entries) {
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

Errors writeZipFile(const QList<ZipEntry> &entries, const QString &zipPath) {
    if (QFileInfo::exists(zipPath) || QFileInfo(zipPath).isSymLink())
        return {makeError(QStringLiteral("permission_denied"),
                          QStringLiteral("%1 is there already; Fugue does not replace it.").arg(zipPath),
                          QStringLiteral("Pick another name."))};
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
    return {};
}

Errors unpackZip(const QString &zipPath, const QString &folder, const UnpackLimits &limits) {
    std::unique_ptr<archive, decltype(&archive_read_free)> in(archive_read_new(), archive_read_free);
    archive_read_support_format_zip(in.get());
    if (archive_read_open_filename(in.get(), QFile::encodeName(zipPath).constData(), 64 * 1024) != ARCHIVE_OK)
        return {notAPackage(zipPath, QString::fromLocal8Bit(archive_error_string(in.get())))};
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
            return {notAPackage(zipPath, QStringLiteral("it names %1, outside its folder").arg(name))};
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
            return {notAPackage(zipPath, QStringLiteral("%1 is a link").arg(name))};
        if (++entries > limits.maxEntries)
            return {notAPackage(zipPath, QStringLiteral("it holds more than %1 files").arg(limits.maxEntries))};
        if (leftOut(name))
            continue;
        const QString path = folder + QLatin1Char('/') + name;
        const mode_t type = archive_entry_filetype(entry);
        if (type == AE_IFDIR) {
            QDir().mkpath(path);
            continue;
        }
        if (type != AE_IFREG)
            return {notAPackage(zipPath, QStringLiteral("%1 is not a file").arg(name))};
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path);
        if (out.exists() || !out.open(QIODevice::WriteOnly))
            return {notAPackage(zipPath, QStringLiteral("%1 cannot be unpacked").arg(name))};
        qint64 size = 0;
        for (;;) {
            const la_ssize_t got = archive_read_data(in.get(), buffer.data(), size_t(buffer.size()));
            if (got < 0)
                return {notAPackage(zipPath, QString::fromLocal8Bit(archive_error_string(in.get())))};
            if (got == 0)
                break;
            size += got;
            unpacked += got;
            // Counted as the bytes come, whatever the entry says of its size.
            if (size > limits.maxFile)
                return {notAPackage(zipPath, QStringLiteral("%1 is over %2 MB").arg(name).arg(limits.maxFile >> 20))};
            if (unpacked > limits.maxTotal)
                return {notAPackage(zipPath, QStringLiteral("it unpacks to more than %1 GB").arg(limits.maxTotal >> 30))};
            if (out.write(buffer.constData(), got) != got)
                return {makeError(writeFailureCode(EIO),
                                 QStringLiteral("Fugue could not unpack %1: %2.").arg(name, out.errorString()))};
        }
    }
    if (read != ARCHIVE_EOF)
        return {notAPackage(zipPath, QString::fromLocal8Bit(archive_error_string(in.get())))};
    return {};
}

} // namespace fugue
