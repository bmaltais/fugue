#pragma once

// How fast a model file samples on a device, measured from real runs (#370, spec 8.9): the speed step
// of the Recommended mark (engine/model_recommend.h).
//
// The helper's progress lines carry the step of a sampling job; HelperProvider times the steps from the
// first to the last it saw (the first step's own time and the model's load are left out) and records
// the seconds per step per megapixel of a job at guidance 1 with no frames, so jobs of other sizes
// compare. The figure is kept per device and model file (its name and size: no hash is read), the
// median of the last few, in `model_speed.json` of the state folder, which every Fugue process
// shares: a record reads the file again before it writes.
//
// A device is where the helper ran: "<backend>:<name>", for example "cuda:NVIDIA GeForce RTX 3090".
// A remote helper's runs are not recorded here: its numbers belong to its own device, and the
// recommendation for it is nothing until it is measured there.

#include <QMutex>
#include <QString>

#include <optional>

namespace fugue {

// The GPU work runs on, as far as the rule needs: the backend of the helper that would run it, the card's
// name, and for NVIDIA its compute capability (INT8 tensor cores from 8.0, Ampere).
struct ComputeDevice {
    QString backend; // "cuda", "vulkan" or "cpu"
    QString name;
    std::optional<double> computeCapability;
    qint64 memoryMb = 0; // what the model files and their working memory have to fit; 0 when not known

    QString id() const { return backend + QLatin1Char(':') + name; }
};

// This machine's, as the local helper runs: found once and kept. No GPU, no helper: backend "cpu".
ComputeDevice localComputeDevice();

class ModelSpeeds {
public:
    explicit ModelSpeeds(QString path = defaultPath());
    static QString defaultPath();

    // Adds one measurement: seconds per step per megapixel of `modelFile` on `device`.
    void record(const QString &device, const QString &modelFile, double secondsPerStepPerMp);
    // The median of the last measurements; nothing when there is none.
    std::optional<double> speed(const QString &device, const QString &modelFile) const;

    // How many measurements are kept per model and device.
    static constexpr int kKept = 5;

private:
    // A file as the store names it: its name and size.
    static QString keyOf(const QString &modelFile);

    QString m_path;
    mutable QMutex m_mutex;
};

} // namespace fugue
