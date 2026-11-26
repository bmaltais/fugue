#pragma once

// Use what you have (#605, spec 2.5, 8.9, 11.10): when a workflow names a model that is not on disk but the same weights are, in
// another file (another quantization, or the same quantization from another publisher), every place that offers a download offers
// that file too. This is the one piece they all call, so they never disagree:
//
//   findOnDisk()   decides: for one missing handle, the file of the same weights (one family, one stem, a file stable-diffusion.cpp
//                  can load), the Recommended one (engine/model_recommend.h) when there are several. Never a file of another
//                  family, never one the engine cannot load, never a file with no readable header.
//   onDiskUses()   decides for every missing model of a workflow's list (the Get ready plan's models, a template's).
//   withUses()     applies: a copy of a document with the handle of each missing model replaced by the file's. A surface that edits
//                  a document live (the node, with undo) sets the one input itself with useHandle(); nothing is ever swapped by
//                  itself and a workflow file is rewritten only when the person saves it.
//
// A file of the same name and quantization as the one wanted that is not the same bytes (the same Qwen3 Q8_0 from another
// publisher) is offered too, marked `otherSource`, with a note that it is not verified to be identical: the weights are the same
// model and the same quantization, which is what the template needs.

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

#include "engine/model_library.h"
#include "engine/model_recommend.h"

namespace fugue {

class Document;
class FileHashes;

struct OnDiskMatch {
    QString name;        // the handle's name: the file under its type folder
    QString path;        // absolute
    QString resource;    // the handle's resource type
    QString tier;        // what people read: "bf16", "Q8_0", "INT8"
    qint64 size = 0;
    bool recommended = false; // chosen among several files of the model
    bool otherSource = false; // the same name and quantization as the wanted file, from another source
};

// A missing model and the file of the same weights that is here.
struct OnDiskUse {
    QJsonObject wanted; // the handle a workflow names (resource, hash, name)
    OnDiskMatch match;

    // "same model, bf16"; "same model, Q8_0, from another source".
    QString sameModelText() const;
    // "Use what you have: z_image_turbo_bf16.safetensors (same model, bf16)".
    QString offerText() const;
    // "the same model is on disk as <path>; set <input> to it, or pass --use-on-disk" (an input may be empty).
    QString cliHint(const QString &input) const;
};

// The quantization of a file as people read it: "bf16", "Q8_0", "INT8", "fp8"; "full-precision" for a safetensors file that says none.
QString quantLabel(const QString &fileName);

// For a model a workflow names that is not on disk: the file of the same weights that is. None when there is none.
std::optional<OnDiskMatch> findOnDisk(const QJsonObject &handle, const ModelLibrary &library, const RecommendContext &context, ModelShapes &shapes);

// findOnDisk() for each of `missing` (ModelLibrary::missing), in order, those with a match.
QList<OnDiskUse> onDiskUses(const QList<MissingModel> &missing, const ModelLibrary &library, const RecommendContext &context, ModelShapes &shapes);

// The handle a use stores: the file's name and `hash` (FileHashes::hash of its path).
QJsonObject useHandle(const OnDiskUse &use, const QString &hash);

// What replaces each missing model, by the wanted handle's hash.
using Uses = QMap<QString, QJsonObject>;

// The uses with their hashes read (a file's hash is remembered, so only a file never hashed is read: call it off the window's
// thread). A file that cannot be read has no entry.
Uses resolveUses(const QList<OnDiskUse> &uses, FileHashes &hashes);

// `value` with every handle whose hash is a key of `uses` replaced, wherever it is nested (a derived model's parts).
QJsonValue withUses(const QJsonValue &value, const Uses &uses);
// A copy of `document` with its nodes' and groups' resource handles replaced. `requires` is left as the workflow wrote it.
Document withUses(const Document &document, const Uses &uses);

} // namespace fugue
