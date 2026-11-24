#include "engine/model_recommend.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <algorithm>

#include "engine/model_family.h"
#include "engine/model_library.h"
#include "engine/registry.h"
#include "engine/model_weights.h"

namespace fugue {

namespace {

bool hasWord(const QStringList &words, const char *word) { return words.contains(QLatin1String(word)); }

QStringList wordsOf(const QString &fileName) { return modelNameWords(fileName); }

} // namespace

bool isInt8(const QString &fileName) { return hasWord(wordsOf(fileName), "int8"); }

QualityTier qualityTier(const QString &fileName) {
    const QStringList words = wordsOf(fileName);
    for (const QString &word : words) {
        // Q8_0 is the GGUF tier of INT8; Q8_1, IQ and the like are not named.
        if (word == QLatin1String("q8"))
            return QualityTier::Q8;
        if (word == QLatin1String("q6"))
            return QualityTier::Q6;
        if (word == QLatin1String("q5"))
            return QualityTier::Q5;
        if (word.startsWith(QLatin1Char('q')) && word.size() == 2 && word.at(1).isDigit())
            return QualityTier::Q4; // q4, q3, q2: the same, worst, tier
        if (word.startsWith(QLatin1String("iq")) || word == QLatin1String("nf4") || word == QLatin1String("int4") ||
            word == QLatin1String("svdq"))
            return QualityTier::Q4;
    }
    if (hasWord(words, "int8"))
        return QualityTier::Q8;
    for (const QString &word : words)
        if (word.startsWith(QLatin1String("fp8")) || word == QLatin1String("e4m3fn") || word == QLatin1String("e5m2"))
            return QualityTier::Fp8;
    for (const char *word : {"bf16", "fp16", "f16", "fp32", "f32"})
        if (hasWord(words, word))
            return QualityTier::Full;
    // No word: a safetensors file is as the author trained it, bf16; a GGUF without one cannot be told.
    return fileName.endsWith(QLatin1String(".safetensors"), Qt::CaseInsensitive) ? QualityTier::Full : QualityTier::Unknown;
}

QString qualityTierName(QualityTier tier) {
    switch (tier) {
    case QualityTier::Full: return QCoreApplication::translate("ModelRecommend", "full-precision");
    case QualityTier::Q8: return QStringLiteral("Q8");
    case QualityTier::Fp8: return QStringLiteral("fp8");
    case QualityTier::Q6: return QStringLiteral("Q6");
    case QualityTier::Q5: return QStringLiteral("Q5");
    case QualityTier::Q4: return QStringLiteral("Q4");
    case QualityTier::Unknown: break;
    }
    return {};
}

bool fitsMemory(qint64 sizeBytes, qint64 otherModelsMb, qint64 memoryMb) {
    if (memoryMb <= 0)
        return true; // memory not known: nothing to drop on
    const qint64 needMb = (sizeBytes / 1000000 + otherModelsMb) * 6 / 5;
    return needMb <= memoryMb;
}

namespace {

struct Entry {
    const RecommendCandidate *candidate;
    QString name;
    QualityTier tier;
    bool int8;
    bool gguf;
};

// A speed rank without a measurement: higher is faster.
int defaultRank(const Entry &entry, const ComputeDevice &device) {
    const bool int8Cores = device.backend == QLatin1String("cuda") && device.computeCapability && *device.computeCapability >= 8.0;
    if (int8Cores)
        return entry.int8 ? 2 : 1;
    return entry.gguf ? 2 : 1;
}

} // namespace

QHash<QString, Recommendation> recommend(const QList<RecommendCandidate> &candidates, const RecommendContext &context) {
    // Groups of the same weights (engine/model_weights.h); a file with no key is in none.
    QHash<QString, QList<Entry>> groups;
    for (const RecommendCandidate &candidate : candidates) {
        const QString name = QFileInfo(candidate.path).fileName();
        const QString key = weightsKey(candidate.shape, name);
        if (key.isEmpty())
            continue;
        groups[key].append(
            Entry{&candidate, name, qualityTier(name), isInt8(name), name.endsWith(QLatin1String(".gguf"), Qt::CaseInsensitive)});
    }

    QHash<QString, Recommendation> result;
    for (const QList<Entry> &group : std::as_const(groups)) {
        // A choice is among files the helper can load.
        const qsizetype loadable = std::count_if(group.begin(), group.end(), [](const Entry &e) { return e.candidate->shape->unloadable.isEmpty(); });
        if (loadable < 2)
            continue;
        // 1. Loadable, and fits the memory.
        QList<Entry> left;
        for (const Entry &entry : group)
            if (entry.candidate->shape->unloadable.isEmpty() && entry.tier != QualityTier::Unknown &&
                fitsMemory(entry.candidate->size, context.otherModelsMb, context.device.backend == QLatin1String("cpu") ? 0 : context.device.memoryMb))
                left << entry;
        if (left.isEmpty())
            continue;
        // 2. The best tier left.
        const QualityTier best = std::max_element(left.begin(), left.end(), [](const Entry &a, const Entry &b) { return a.tier < b.tier; })->tier;
        left.erase(std::remove_if(left.begin(), left.end(), [best](const Entry &e) { return e.tier != best; }), left.end());
        // 3. The fastest: measured when every one has a measurement, else by the backend's default.
        bool measured = left.size() > 1;
        QHash<QString, double> speeds;
        if (context.speeds)
            for (const Entry &entry : std::as_const(left)) {
                if (const std::optional<double> speed = context.speeds->speed(context.device.id(), entry.candidate->path))
                    speeds.insert(entry.candidate->path, *speed);
                else
                    measured = false;
            }
        else
            measured = false;
        const auto faster = [&](const Entry &a, const Entry &b) {
            if (measured && speeds.value(a.candidate->path) != speeds.value(b.candidate->path))
                return speeds.value(a.candidate->path) < speeds.value(b.candidate->path);
            if (!measured && defaultRank(a, context.device) != defaultRank(b, context.device))
                return defaultRank(a, context.device) > defaultRank(b, context.device);
            if (a.candidate->size != b.candidate->size)
                return a.candidate->size < b.candidate->size;
            return a.name < b.name;
        };
        const Entry &winner = *std::min_element(left.begin(), left.end(), faster);
        const QString level = qualityTierName(best);
        QString reason;
        if (left.size() == 1)
            reason = QCoreApplication::translate("ModelRecommend", "Recommended: best quality that fits here (%1 level)").arg(level);
        else if (measured)
            reason = QCoreApplication::translate("ModelRecommend", "Recommended: fastest here at %1-level quality").arg(level);
        else
            reason = QCoreApplication::translate("ModelRecommend", "Recommended: expected to be fastest here at %1-level quality").arg(level);
        result.insert(winner.candidate->path, Recommendation{true, reason});
    }
    return result;
}

QString familyKindOf(const QString &resourceType) {
    const QString kind = resourceType.section(QLatin1Char('/'), -1);
    return kind == QLatin1String("model") ? QStringLiteral("checkpoint") : kind;
}

QHash<QString, ModelMark> markModels(const QString &resourceType, const QList<MarkedFile> &files, const RecommendContext &context,
                                     ModelShapes &shapes) {
    QHash<QString, ModelMark> marks;
    QList<RecommendCandidate> candidates;
    const QString kind = familyKindOf(resourceType);
    for (const MarkedFile &file : files) {
        const QString family = modelFamily(kind, QFileInfo(file.path).fileName());
        if (family.isEmpty())
            continue;
        marks.insert(file.path, ModelMark{family, false, {}});
        candidates.append({file.path, file.size, shapes.of(file.path)});
    }
    const QHash<QString, Recommendation> picked = recommend(candidates, context);
    for (auto it = picked.begin(); it != picked.end(); ++it) {
        marks[it.key()].recommended = true;
        marks[it.key()].reason = it->reason;
    }
    return marks;
}

QString suggestOnDisk(const QJsonObject &handle, const ModelLibrary &library, const RecommendContext &context, ModelShapes &shapes) {
    const QString resource = handle.value(QStringLiteral("resource")).toString();
    const QString wanted = handle.value(QStringLiteral("name")).toString();
    const std::optional<PortType> type = PortType::parse(resource);
    if (!type || modelFamily(familyKindOf(resource), wanted).isEmpty())
        return {};
    const QString family = modelFamily(familyKindOf(resource), wanted);
    const QString stem = modelStem(wanted);
    QList<ModelFile> same;
    for (const ModelFile &file : library.list(*type))
        if (modelFamily(familyKindOf(resource), file.name) == family && modelStem(file.name) == stem)
            same << file;
    if (same.isEmpty())
        return {};
    if (same.size() > 1) {
        QList<MarkedFile> files;
        for (const ModelFile &file : std::as_const(same))
            files.append({file.path, file.size});
        const QHash<QString, ModelMark> marks = markModels(resource, files, context, shapes);
        for (const ModelFile &file : std::as_const(same))
            if (marks.value(file.path).recommended)
                return file.name;
    }
    return same.first().name;
}

} // namespace fugue
