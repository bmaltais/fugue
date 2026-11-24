#include "engine/model_speed.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutexLocker>
#include <QProcess>

#include <algorithm>

#include "engine/atomic_file.h"
#include "engine/gpu_memory.h"
#include "engine/helper_provider.h"
#include "engine/json.h"
#include "engine/paths.h"

namespace fugue {

QString ModelSpeeds::defaultPath() { return stateDir() + QStringLiteral("/model_speed.json"); }

ModelSpeeds::ModelSpeeds(QString path) : m_path(std::move(path)) {}

QString ModelSpeeds::keyOf(const QString &modelFile) {
    const QFileInfo info(modelFile);
    return QStringLiteral("%1|%2").arg(info.fileName()).arg(info.size());
}

namespace {

QJsonObject load(const QString &path) {
    const Result<QJsonObject> read = readJsonObject(path);
    return read ? read.value() : QJsonObject();
}

} // namespace

void ModelSpeeds::record(const QString &device, const QString &modelFile, double secondsPerStepPerMp) {
    if (!(secondsPerStepPerMp > 0) || !QFileInfo::exists(modelFile))
        return;
    const QMutexLocker lock(&m_mutex);
    QJsonObject root = load(m_path);
    QJsonObject devices = root.value(QStringLiteral("devices")).toObject();
    QJsonObject models = devices.value(device).toObject();
    const QString key = keyOf(modelFile);
    QJsonArray seconds = models.value(key).toObject().value(QStringLiteral("seconds")).toArray();
    seconds.append(secondsPerStepPerMp);
    while (seconds.size() > kKept)
        seconds.removeFirst();
    models.insert(key, QJsonObject{{"seconds", seconds}});
    devices.insert(device, models);
    root.insert(QStringLiteral("format"), 1);
    root.insert(QStringLiteral("devices"), devices);
    makeFolder(QFileInfo(m_path).absolutePath());
    writeJsonFile(m_path, root);
}

std::optional<double> ModelSpeeds::speed(const QString &device, const QString &modelFile) const {
    const QMutexLocker lock(&m_mutex);
    const QJsonArray seconds = load(m_path)
                                   .value(QStringLiteral("devices")).toObject()
                                   .value(device).toObject()
                                   .value(keyOf(modelFile)).toObject()
                                   .value(QStringLiteral("seconds")).toArray();
    QList<double> values;
    for (const QJsonValue &value : seconds)
        values << value.toDouble();
    if (values.isEmpty())
        return std::nullopt;
    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
}

ComputeDevice localComputeDevice() {
    static const ComputeDevice device = [] {
        ComputeDevice found;
        const bool nvidia = hasNvidiaGpu();
        const QString helper = helperProgram(nvidia);
        // No helper found (a source checkout without one built): an NVIDIA card is a CUDA device, the helper
        // Fugue prefers there.
        found.backend = helper.contains(QLatin1String("cuda")) ? QStringLiteral("cuda")
                        : helper.contains(QLatin1String("vulkan")) ? QStringLiteral("vulkan")
                        : nvidia                                   ? QStringLiteral("cuda")
                                                                   : QStringLiteral("cpu");
        if (nvidia) {
            QProcess smi;
            smi.start(QStringLiteral("nvidia-smi"), {QStringLiteral("--query-gpu=name,compute_cap,memory.total"),
                                                     QStringLiteral("--format=csv,noheader,nounits")});
            if (smi.waitForFinished(5000) && smi.exitCode() == 0) {
                const QStringList fields = QString::fromUtf8(smi.readAllStandardOutput()).split(QLatin1Char('\n')).value(0).split(QLatin1Char(','));
                if (fields.size() >= 3) {
                    found.name = fields.at(0).trimmed();
                    bool ok = false;
                    if (const double capability = fields.at(1).trimmed().toDouble(&ok); ok)
                        found.computeCapability = capability;
                    found.memoryMb = fields.at(2).trimmed().toLongLong();
                }
            }
        }
        if (found.name.isEmpty() || found.memoryMb == 0) {
            if (const std::optional<GpuDevice> gpu = GpuMemoryProbe().device()) {
                if (found.name.isEmpty())
                    found.name = gpu->name;
                if (found.memoryMb == 0)
                    found.memoryMb = gpu->kind == GpuDevice::Kind::Unified
                                         ? freeGpuMemoryMb(*gpu, memAvailableMb()).value_or(gpu->heapMb)
                                         : gpu->heapMb;
            }
        }
        if (found.name.isEmpty())
            found.name = QStringLiteral("unknown");
        return found;
    }();
    return device;
}

} // namespace fugue
