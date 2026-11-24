#pragma once

// Which files are the same weights (#370, spec 2.5): one model in several quantizations. The family
// (engine/model_family.h) says what a model is called; this says two files of it are one model.
//
// Two files are the same weights when they are in one family, have the same header figures
// (engine/model_shape.h) and the same stem: the file's name without its quantization and format
// words. The stem is what tells a distilled model from its base, which share every shape
// (flux-2-klein-9b and flux-2-klein-base-9b), and a model from its fine-tune or merge. There is no
// hash list. A file with no header, or in no family, is never the same weights as another.

#include <QString>
#include <QStringList>

#include <optional>

#include "engine/model_shape.h"

namespace fugue {

// The words of a file's name: lower case, no folder or extension, split at every character that is not
// a letter or digit ("Flux-2_klein-9b-Q8_0.gguf" is flux 2 klein 9b q8 0).
QStringList modelNameWords(const QString &fileName);

// A file's name as one model: the words without quantization or format ones (bf16, fp8, Q5_K_M, int8,
// convrot, gguf, ...), joined. "flux-2-klein-9b-Q8_0.gguf" and "flux_2_klein_9b.int8_convrot.safetensors"
// are both "flux2klein9b".
QString modelStem(const QString &fileName);

// The key two files share when they are the same weights; empty when the file has none (no header, or
// no family).
QString weightsKey(const std::optional<ModelShape> &shape, const QString &fileName);

} // namespace fugue
