#pragma once

// Which file of a model to pick (#370, spec 2.5, 8.9): among the files of one model in several
// quantizations, mark one Recommended with a reason. Only a mark: nothing is picked by itself, and a
// template keeps its file until the person swaps it.
//
// The rule, in order, for each group of files that are the same weights (engine/model_weights.h: one
// family, one stem and shape; a file the table does not know is never in a group, one that stable-diffusion.cpp
// cannot load is left out):
//   1. Fits the memory: a file whose weights and the workflow's other models, with a fifth more for the
//      model's working memory (8.10's figure), do not fit the device's memory is dropped.
//   2. Quality tier: keep the best tier that is left: bf16 or fp16, then Q8_0 and INT8 (one tier), fp8,
//      Q6_K, Q5, Q4. Fewer bits only when a better tier does not fit. The tier comes from the file's name
//      (a table here, no per-file list); a .safetensors file with no quantization word is taken as bf16, a
//      GGUF with none has no tier and is never recommended.
//   3. Speed in the tier: the measured seconds per step on this device (engine/model_speed.h) when every
//      file left has one, else the per-backend default: INT8 on CUDA with compute capability 8.0 or more
//      (INT8 tensor cores), GGUF on Vulkan and on CPU, where INT8 is untested. Remaining ties: the smaller
//      file, then the name.
// A group of one file has nothing to choose from: no mark.
//
// A remote helper has its own device: ask with its device and its speeds, or not at all.

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

#include "engine/model_shape.h"
#include "engine/model_speed.h"

namespace fugue {

// The quality of a quantization: higher is better; INT8 is as good as Q8.
enum class QualityTier { Unknown = 0, Q4, Q5, Q6, Fp8, Q8, Full };

// A file's tier from its name and whether it is a safetensors file (.gguf otherwise).
QualityTier qualityTier(const QString &fileName);
// "full-precision", "Q8", "fp8", "Q6", "Q5", "Q4": what the reason says.
QString qualityTierName(QualityTier tier);

// Whether the name says INT8 (ConvRot): the quantization that needs INT8 tensor cores to be fast.
bool isInt8(const QString &fileName);

struct RecommendCandidate {
    QString path;
    qint64 size = 0;
    std::optional<ModelShape> shape; // read from the header; none: unknown, never grouped
};

struct RecommendContext {
    ComputeDevice device;
    // The other models of the workflow that share the device's memory (the text encoder and VAE of a
    // diffusion model, in MB); 0 when none is known.
    qint64 otherModelsMb = 0;
    // Where the measured speeds are looked up; none: only the default per backend.
    const ModelSpeeds *speeds = nullptr;
};

struct Recommendation {
    bool recommended = false;
    QString reason; // "Recommended: fastest here at Q8-level quality"
};

// The recommended file of each group of `candidates`, by path (a path that is not recommended is not in
// the result).
QHash<QString, Recommendation> recommend(const QList<RecommendCandidate> &candidates, const RecommendContext &context);

// What a picker, Settings or `fugue model list` says of a model file: its family (engine/model_family.h,
// by name; empty when none) and whether it is the Recommended one of its model, with the reason.
struct ModelMark {
    QString family;
    bool recommended = false;
    QString reason;
};

struct MarkedFile {
    QString path;
    qint64 size = 0;
};

// The marks of `files` of the resource type `resourceType` ("fugue/diffusion-model"), by path. Reads the
// header of the files of a known family only (ModelShapes keeps what it read). A type with no families
// (a LoRA) gets none.
QHash<QString, ModelMark> markModels(const QString &resourceType, const QList<MarkedFile> &files, const RecommendContext &context,
                                     ModelShapes &shapes);

class ModelLibrary;

// For a model a template names that is not on disk: the file of the same weights (one family, one stem:
// another quantization of it) that is, as the file name under its folder, and the Recommended one when
// there are several; empty when none is. A template keeps its file: this is only said on its card.
QString suggestOnDisk(const QJsonObject &handle, const ModelLibrary &library, const RecommendContext &context, ModelShapes &shapes);

// A resource type's kind as the family table names it: "fugue/model" is a checkpoint.
QString familyKindOf(const QString &resourceType);

// Whether a file's weights and `otherModelsMb`, with a fifth for working memory, fit `memoryMb`.
bool fitsMemory(qint64 sizeBytes, qint64 otherModelsMb, qint64 memoryMb);

} // namespace fugue
