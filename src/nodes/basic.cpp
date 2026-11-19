// The basic nodes (spec 14.1, ticket #13): Text, Number, Load Image, Save Image, Preview.

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "engine/atomic_file.h"
#include "engine/embed.h"
#include "engine/file_hashes.h"
#include "engine/json.h"
#include "engine/media.h"
#include "engine/paths.h"
#include "engine/share_copy.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

namespace {

// Text and Number give back what was typed in them.
class PassKernel : public NodeKernel {
public:
    explicit PassKernel(QString port) : m_port(std::move(port)) {}
    Result<Values> run(const NodeContext &context) override { return Values{{m_port, context.input(m_port)}}; }

private:
    const QString m_port;
};

class LoadImageKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        // The scheduler has expanded "~" (paths.h), so the file is the one the key was made from.
        QString file = context.input("file").text();
        if (context.roots) {
            // Read the path that was checked, so check and use cannot disagree.
            const Result<QString> readable = context.roots->checkRead(file, QStringLiteral("file"));
            if (!readable)
                return readable.errors();
            file = readable.value();
        }
        if (!QFileInfo(file).isFile())
            return makeError(QStringLiteral("file_not_found"), QStringLiteral("No image file at %1.").arg(file),
                             QStringLiteral("Pick the file again in the node."));
        const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, file, *context.hashes);
        if (!image)
            return image.errors();
        return Values{{"image", Value(image.value())},
                      {"width", Value(image->width())},
                      {"height", Value(image->height())}};
    }
};

// The workflow text Save Image embeds: the share copy (11.5) with a `run` section holding
// what was used upstream (11.7). `embed` is full, no-prompts or none. Empty for none, and
// empty with a warning when the run did not start from a document (a compiled graph), so
// the image is saved and the run says why it holds no workflow.
Result<QByteArray> workflowToEmbed(const NodeContext &context, const QString &embed) {
    if (embed == QLatin1String("none"))
        return QByteArray();
    if (context.document.isEmpty() || !context.registry) {
        Error warning = makeError(QStringLiteral("workflow_not_embedded"),
                                  QStringLiteral("The image holds no workflow: the run started from a compiled graph, "
                                                 "which has none to embed."),
                                  QStringLiteral("Run the workflow file itself to embed it."));
        warning.severity = Error::Severity::Warning;
        return Result<QByteArray>(QByteArray(), Errors{warning});
    }
    ShareOptions options;
    options.withoutPrompts = embed == QLatin1String("no-prompts");
    QJsonObject document = shareCopy(context.document, *context.registry, options);
    document.insert("run", QJsonObject{{"nodes", context.usedUpstream}});
    return compactJson(document);
}

// The file names a save tries in turn: `<name>.<ext>`, then `<name>-2.<ext>`, `-3`, ...
QString numberedName(const QString &name, int n, const QString &extension) {
    return n == 1 ? QStringLiteral("%1.%2").arg(name, extension) : QStringLiteral("%1-%2.%3").arg(name).arg(n).arg(extension);
}

// Writes `bytes` under the first free numbered name in `folder` and returns its path. The
// name is claimed in the same step as the file appears, so a save never overwrites a file
// (it may be the user's), not even one another Save node wrote a moment before.
Result<QString> saveAsNew(const QString &folder, const QString &name, const QString &extension, const QByteArray &bytes) {
    PartialFile::removeStale(folder); // what a save that died here left behind
    PartialFile partial(folder, numberedName(name, 1, extension));
    if (const Errors written = partial.write(bytes, QDir(folder).filePath(numberedName(name, 1, extension)));
        hasErrors(written))
        return written;
    for (int n = 1; n <= 100000; ++n) {
        const QString target = QDir(folder).filePath(numberedName(name, n, extension));
        const Result<bool> committed = partial.commit(target, Overwrite::Never);
        if (!committed)
            return committed.errors();
        if (*committed)
            return target;
    }
    return makeError(QStringLiteral("node_failed"), QStringLiteral("No free name for %1 in %2.").arg(name, folder),
                     QStringLiteral("Pick another name or folder in the node."));
}

class SaveImageKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const QString name = context.input("name").text().trimmed();
        if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.startsWith(QLatin1Char('.')))
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("name"),
                                  QStringLiteral("\"%1\" cannot be a file name.").arg(name),
                                  QStringLiteral("Use a plain name without folders; the folder input says where."));
        const QString format = context.input("format").text();
        // A file name holds at most 255 bytes (NAME_MAX) with its extension.
        if (const qsizetype bytes = QFile::encodeName(name + QLatin1Char('.') + format).size(); bytes > 255)
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("name"),
                                  QStringLiteral("The name is too long for a file name: %1 bytes with .%2, at most 255.")
                                      .arg(bytes)
                                      .arg(format),
                                  QStringLiteral("Use a shorter name."));
        const QString embed = context.input("embed").text();

        // Relative folders are under the output folder, so a workflow made elsewhere cannot
        // write outside it by accident.
        QString folder = context.input("folder").text().trimmed();
        if (folder.isEmpty()) {
            folder = outputDir();
        } else if (QDir::isRelativePath(folder)) {
            folder = outputDir() + QLatin1Char('/') + folder;
            if (!isInside(canonicalPath(folder), canonicalPath(outputDir())))
                return makeInputError(QStringLiteral("permission_denied"), QStringLiteral("folder"),
                                      QStringLiteral("%1 leads out of the output folder %2.").arg(context.input("folder").text().trimmed(), outputDir()),
                                      QStringLiteral("A relative folder is under the output folder; write an absolute one to go elsewhere."));
        }
        // The folder as the system resolves it (links followed where they stand, then `..`),
        // so making it, writing in it and checking it are about one folder. QDir::mkpath
        // would clean `link/..` away on paper while the write follows the link.
        folder = canonicalPath(folder);
        if (folder.isEmpty())
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("folder"),
                                  QStringLiteral("The folder %1 goes through links that loop.").arg(context.input("folder").text().trimmed()));
        if (context.roots) {
            // Write into the path that was checked.
            const Result<QString> writable = context.roots->checkWrite(folder, QStringLiteral("folder"));
            if (!writable)
                return writable.errors();
            folder = writable.value();
        }
        if (Errors made = makeFolder(folder); !made.isEmpty()) {
            made.first().input = QStringLiteral("folder");
            made.first().hint = QStringLiteral("Pick another folder in the node.");
            return made;
        }

        // The pixels as a PNG the engine can write (8 or 16 bit), made from a copy so an input
        // file is never the one read and written.
        const Result<vips::VImage> pixels = context.input("image").media().image();
        if (!pixels)
            return pixels.errors();
        const Result<MediaHandle> memory = MediaHandle::fromImage(pixels.value());
        if (!memory)
            return memory.errors();
        const Result<MediaHandle> scratch = memory->saved(context.workDir, QStringLiteral("source"), *context.hashes);
        if (!scratch)
            return scratch.errors();

        const Result<QByteArray> embedded = workflowToEmbed(context, embed);
        const QByteArray workflow = embedded.value();
        QByteArray bytes;
        if (format == QLatin1String("webp")) {
            // From the PNG just written, so 16-bit and float images take the same road to 8 bits.
            const Result<MediaHandle> png = MediaHandle::probe(MediaType::Image, scratch->path(), *context.hashes);
            if (!png)
                return png.errors();
            const Result<vips::VImage> decoded = png->image();
            if (!decoded)
                return decoded.errors();
            const Result<QByteArray> webp = webpWithWorkflow(decoded.value(), workflow);
            if (!webp)
                return webp.errors();
            bytes = webp.value();
        } else {
            QFile source(scratch->path());
            if (!source.open(QIODevice::ReadOnly))
                return makeError(QStringLiteral("internal"), QStringLiteral("Cannot read back %1.").arg(scratch->path()));
            bytes = source.readAll();
            source.close();
            if (!workflow.isEmpty()) {
                const Result<QByteArray> withWorkflow = pngWithWorkflow(bytes, workflow);
                if (!withWorkflow)
                    return withWorkflow.errors();
                bytes = withWorkflow.value();
            }
        }
        QFile::remove(scratch->path());
        const Result<QString> target = saveAsNew(folder, name, format, bytes);
        if (!target)
            return target.errors();

        const Result<MediaHandle> file = MediaHandle::probe(MediaType::Image, *target, *context.hashes);
        if (!file)
            return file.errors();
        return Result<Values>(Values{{"file", Value(file.value())}}, embedded.warnings());
    }
};

// Preview does nothing itself: what it shows is its input, which the scheduler
// reports for a sink with no outputs (`shown` in node.done, node.cached and run.json).
class PreviewKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &) override { return Values{}; }
};

} // namespace

void addBasicKernels(Kernels &kernels) {
    kernels.add("fugue/text", std::make_shared<PassKernel>("text"));
    kernels.add("fugue/number", std::make_shared<PassKernel>("value"));
    kernels.add("fugue/load-image", std::make_shared<LoadImageKernel>());
    kernels.add("fugue/save-image", std::make_shared<SaveImageKernel>());
    kernels.add("fugue/preview", std::make_shared<PreviewKernel>());
}

} // namespace fugue::nodes
