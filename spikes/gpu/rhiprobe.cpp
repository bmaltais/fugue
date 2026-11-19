// Spike 04, part 1: Qt RHI compute in a CLI process with no window.
// Usage: rhiprobe vulkan|opengl|null   (run with QT_QPA_PLATFORM=offscreen, or minimal)
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QVulkanInstance>
#include <rhi/qrhi.h>
#include <cstdio>
#include <memory>

static QShader load(const char *path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication app(argc, argv);
    const QByteArray backend = argc > 1 ? argv[1] : "vulkan";
    const char *qsb = argc > 2 ? argv[2] : "double.comp.qsb";
    printf("platform plugin: %s, backend: %s\n", qPrintable(QGuiApplication::platformName()), backend.constData());

    // Destroyed in reverse order: the QRhi must go before the instance and surface it uses.
    QVulkanInstance inst;
    std::unique_ptr<QOffscreenSurface> surface;
    std::unique_ptr<QRhi> rhi;
    if (backend == "vulkan") {
        inst.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (!inst.create()) { printf("QVulkanInstance::create failed: %d\n", inst.errorCode()); return 1; }
        QRhiVulkanInitParams p;
        p.inst = &inst;
        rhi.reset(QRhi::create(QRhi::Vulkan, &p));
    } else if (backend == "opengl") {
        surface.reset(QRhiGles2InitParams::newFallbackSurface());
        QRhiGles2InitParams p;
        p.fallbackSurface = surface.get();
        rhi.reset(QRhi::create(QRhi::OpenGLES2, &p));
    } else {
        QRhiNullInitParams p;
        rhi.reset(QRhi::create(QRhi::Null, &p));
    }
    if (!rhi) { printf("QRhi::create failed\n"); return 1; }
    printf("device: %s, compute supported: %s\n", rhi->driverInfo().deviceName.constData(),
           rhi->isFeatureSupported(QRhi::Compute) ? "yes" : "no");
    if (!rhi->isFeatureSupported(QRhi::Compute)) return 2;

    const int N = 1 << 20;
    std::unique_ptr<QRhiBuffer> buf(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, N * 4));
    buf->create();
    std::unique_ptr<QRhiShaderResourceBindings> srb(rhi->newShaderResourceBindings());
    srb->setBindings({QRhiShaderResourceBinding::bufferLoadStore(0, QRhiShaderResourceBinding::ComputeStage, buf.get())});
    srb->create();
    QShader cs = load(qsb);
    if (!cs.isValid()) { printf("bad shader %s\n", qsb); return 1; }
    std::unique_ptr<QRhiComputePipeline> pipe(rhi->newComputePipeline());
    pipe->setShaderStage({QRhiShaderStage::Compute, cs});
    pipe->setShaderResourceBindings(srb.get());
    if (!pipe->create()) { printf("pipeline create failed\n"); return 1; }

    QRhiCommandBuffer *cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) { printf("beginOffscreenFrame failed\n"); return 1; }
    cb->beginComputePass();
    cb->setComputePipeline(pipe.get());
    cb->setShaderResources();
    cb->dispatch(N / 256, 1, 1);
    QRhiReadbackResult rb;
    QRhiResourceUpdateBatch *u = rhi->nextResourceUpdateBatch();
    u->readBackBuffer(buf.get(), 0, N * 4, &rb);
    cb->endComputePass(u);
    rhi->endOffscreenFrame();

    if (backend == "null") { printf("null backend: runs nothing, result not checked\n"); return 0; }
    if (rb.data.size() != N * 4) { printf("readback size %lld\n", (long long)rb.data.size()); return 1; }
    const quint32 *v = reinterpret_cast<const quint32 *>(rb.data.constData());
    int bad = 0;
    for (int i = 0; i < N; i++) if (v[i] != quint32(i) * 2u + 1u) bad++;
    printf("compute dispatch (1M uints): %s\n", bad ? "WRONG RESULT" : "ok");
    return bad ? 1 : 0;
}
