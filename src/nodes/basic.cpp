// The basic nodes (spec 14.1, ticket #13): Text, Number, Load Image, Save Image, Preview; and
// Load Video and Load Audio (14.2, #28).

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "engine/embed.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "nodes/kernels.h"
#include "nodes/mask_input.h"
#include "nodes/paint_input.h"
#include "nodes/pixel_io.h"
#include "nodes/save_files.h"

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

// Load Image, Load Video and Load Audio: one file, probed into a handle of the node's media type
// (2.4) and described by the numbers the type carries. A video or audio file is only probed, never
// decoded (media_video.cpp).
class LoadMediaKernel : public NodeKernel {
public:
    explicit LoadMediaKernel(MediaType type) : m_type(type) {}

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
        const QString kind = mediaTypeName(m_type);
        if (!QFileInfo(file).isFile())
            return makeError(QStringLiteral("file_not_found"), QStringLiteral("No %1 file at %2.").arg(kind, file),
                             QStringLiteral("Pick the file again in the node."));
        const Result<MediaHandle> media = MediaHandle::probe(m_type, file, *context.hashes);
        if (!media)
            return media.errors();
        Values values{{kind, Value(media.value())}};
        switch (m_type) {
        case MediaType::Image:
            values.insert("width", Value(media->width()));
            values.insert("height", Value(media->height()));
            return withMask(context, media.value(), std::move(values));
        case MediaType::Video:
            values.insert("width", Value(media->width()));
            values.insert("height", Value(media->height()));
            values.insert("fps", Value(media->fps()));
            values.insert("duration", Value(media->duration()));
            break;
        case MediaType::Audio:
            values.insert("duration", Value(media->duration()));
            values.insert("sample_rate", Value(media->sampleRate()));
            break;
        }
        return values;
    }

private:
    // Load Image 1.1's `mask` output (14.4), beside its image: a painted mask, else 1 - alpha, else 0.
    static Result<Values> withMask(const NodeContext &context, const MediaHandle &picture, Values values) {
        Errors warnings;
        Result<Pixels> mask = loadImageMask(context, picture, warnings);
        if (!mask.ok())
            return Result<Values>(mask.errors());
        const Result<Values> made = namedOutput(std::move(mask.value()), "mask");
        if (!made.ok())
            return made;
        values.insert("mask", made.value().value("mask"));
        return Result<Values>(values, warnings);
    }

    const MediaType m_type;
};

class SaveImageKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const QString format = context.input("format").text();
        const Result<SaveLocation> where = saveLocation(context, format);
        if (!where)
            return where.errors();
        const QString embed = context.input("embed").text();

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

        const Result<QByteArray> embedded = workflowToEmbed(context, embed, QStringLiteral("image"));
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
        const Result<QString> target = saveAsNew(where.value(), format, bytes);
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
    kernels.add("fugue/load-image", std::make_shared<LoadMediaKernel>(MediaType::Image));
    kernels.add("fugue/load-video", std::make_shared<LoadMediaKernel>(MediaType::Video));
    kernels.add("fugue/load-audio", std::make_shared<LoadMediaKernel>(MediaType::Audio));
    kernels.add("fugue/save-image", std::make_shared<SaveImageKernel>());
    kernels.add("fugue/preview", std::make_shared<PreviewKernel>());
    addMaskInputKernels(kernels);
    addPaintInputKernels(kernels);
}

} // namespace fugue::nodes
