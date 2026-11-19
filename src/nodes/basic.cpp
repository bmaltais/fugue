// The basic nodes (spec 14.1, ticket #13): Text, Number, Load Image, Save Image, Preview.

#include <QDir>
#include <QFile>
#include <QFileInfo>

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

// "~" and "~/..." mean the home folder, as the file picker's paths and hand-written
// workflows both use it.
QString expandHome(const QString &path) {
    if (path == QLatin1String("~"))
        return QDir::homePath();
    if (path.startsWith(QLatin1String("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}

class LoadImageKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const QString file = expandHome(context.input("file").text());
        if (!QFileInfo(file).isFile())
            return makeError(QStringLiteral("file_not_found"), QStringLiteral("No image file at %1.").arg(file),
                             QStringLiteral("Pick the file again in the node."));
        const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, file, *context.hashes);
        if (!image)
            return image.error();
        return Values{{"image", Value(image.value())},
                      {"width", Value(image->width())},
                      {"height", Value(image->height())}};
    }
};

// The workflow text Save Image embeds, or empty when the node embeds none or the run did not
// start from a document: the share copy (11.5) with a `run` section holding what was used
// upstream (11.7). `embed` is full, no-prompts or none.
QByteArray workflowToEmbed(const NodeContext &context, const QString &embed) {
    if (embed == QLatin1String("none") || context.document.isEmpty() || !context.registry)
        return {};
    ShareOptions options;
    options.withoutPrompts = embed == QLatin1String("no-prompts");
    QJsonObject document = shareCopy(context.document, *context.registry, options);
    document.insert("run", QJsonObject{{"nodes", context.used}});
    return compactJson(document);
}

Error badInput(const QString &input, const QString &message, const QString &hint = {}) {
    Error error = makeError(QStringLiteral("node_failed"), message, hint);
    error.input = input;
    return error;
}

// A file name that is free in `dir`: `<name>.<ext>`, else `<name>-2.<ext>`, `-3`, ... A save
// never overwrites a file (it may be the user's).
QString freeName(const QDir &dir, const QString &name, const QString &extension) {
    QString candidate = QStringLiteral("%1.%2").arg(name, extension);
    for (int n = 2; dir.exists(candidate); ++n)
        candidate = QStringLiteral("%1-%2.%3").arg(name).arg(n).arg(extension);
    return candidate;
}

class SaveImageKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const QString name = context.input("name").text().trimmed();
        if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.startsWith(QLatin1Char('.')))
            return badInput("name", QStringLiteral("\"%1\" cannot be a file name.").arg(name),
                            QStringLiteral("Use a plain name without folders; the folder input says where."));
        const QString format = context.input("format").text();
        const QString embed = context.input("embed").text();

        // Relative folders are under the output folder, so a workflow made elsewhere cannot
        // write outside it by accident.
        QString folder = expandHome(context.input("folder").text().trimmed());
        if (folder.isEmpty())
            folder = outputDir();
        else if (QDir::isRelativePath(folder))
            folder = outputDir() + QLatin1Char('/') + folder;
        if (!QDir().mkpath(folder))
            return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot create the folder %1.").arg(folder),
                             QStringLiteral("Pick another folder in the node."));

        // The pixels as a PNG the engine can write (8 or 16 bit), made from a copy so an input
        // file is never the one read and written.
        const Result<vips::VImage> pixels = context.input("image").media().image();
        if (!pixels)
            return pixels.error();
        const Result<MediaHandle> memory = MediaHandle::fromImage(pixels.value());
        if (!memory)
            return memory.error();
        const Result<MediaHandle> scratch = memory->saved(context.workDir, QStringLiteral("source"), *context.hashes);
        if (!scratch)
            return scratch.error();

        const QByteArray workflow = workflowToEmbed(context, embed);
        const QDir dir(folder);
        const QString fileName = freeName(dir, name, format);
        const QString target = dir.filePath(fileName);

        if (format == QLatin1String("webp")) {
            // From the PNG just written, so 16-bit and float images take the same road to 8 bits.
            const Result<MediaHandle> png = MediaHandle::probe(MediaType::Image, scratch->path(), *context.hashes);
            if (!png)
                return png.error();
            const Result<vips::VImage> decoded = png->image();
            if (!decoded)
                return decoded.error();
            const Errors written = writeWebpWithWorkflow(decoded.value(), target, workflow);
            if (hasErrors(written))
                return written.first();
        } else {
            QFile source(scratch->path());
            if (!source.open(QIODevice::ReadOnly))
                return makeError(QStringLiteral("internal"), QStringLiteral("Cannot read back %1.").arg(scratch->path()));
            QByteArray bytes = source.readAll();
            source.close();
            if (!workflow.isEmpty()) {
                const Result<QByteArray> embedded = pngWithWorkflow(bytes, workflow);
                if (!embedded)
                    return embedded.error();
                bytes = embedded.value();
            }
            QFile out(dir.filePath(QStringLiteral(".") + fileName + QStringLiteral(".part")));
            if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) {
                out.remove();
                return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot write %1.").arg(target));
            }
            out.close();
            if (!out.rename(target)) {
                out.remove();
                return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot write %1.").arg(target));
            }
        }
        QFile::remove(scratch->path());

        const Result<MediaHandle> file = MediaHandle::probe(MediaType::Image, target, *context.hashes);
        if (!file)
            return file.error();
        return Values{{"file", Value(file.value())}};
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
