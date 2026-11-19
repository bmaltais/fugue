// The basic nodes (spec 14.1, ticket #13): Text, Number, Load Image, Save Image, Preview.

#include <QDir>
#include <QFileInfo>

#include "engine/file_hashes.h"
#include "engine/media.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

namespace {

// Text and Number give back what was typed in them.
class PassKernel : public NodeKernel {
public:
    explicit PassKernel(QString port) : m_port(std::move(port)) {}
    Result<Values> run(const NodeContext &context) override { return Values{{m_port, context.input(m_port)}}; }

private:
    QString m_port;
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
    kernels.add("fugue/preview", std::make_shared<PreviewKernel>());
}

} // namespace fugue::nodes
