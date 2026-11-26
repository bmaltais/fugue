#include "engine/model_use.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/model_family.h"
#include "engine/model_weights.h"

namespace fugue {

QString quantLabel(const QString &fileName) {
    static const QRegularExpression word(QStringLiteral("(?<![a-z0-9])(bf16|fp16|fp32|fp8\\w*|int8|nf4|i?q\\d\\w*)(?![a-z])"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QString base = QFileInfo(fileName).completeBaseName();
    const QRegularExpressionMatch match = word.match(base);
    if (!match.hasMatch())
        return qualityTier(fileName) == QualityTier::Full ? qualityTierName(QualityTier::Full) : QString();
    const QString text = match.captured(1);
    const QString lower = text.toLower();
    if (lower == QLatin1String("int8"))
        return QStringLiteral("INT8");
    return lower.startsWith(QLatin1Char('q')) || lower.startsWith(QLatin1String("iq")) ? text.toUpper() : lower;
}

QString OnDiskUse::sameModelText() const {
    QStringList parts{QCoreApplication::translate("ModelUse", "same model")};
    if (!match.tier.isEmpty())
        parts << match.tier;
    if (match.otherSource)
        parts << QCoreApplication::translate("ModelUse", "from another source");
    return parts.join(QStringLiteral(", "));
}

QString OnDiskUse::offerText() const {
    return QCoreApplication::translate("ModelUse", "Use what you have: %1 (%2)").arg(match.name, sameModelText());
}

QString OnDiskUse::cliHint(const QString &input) const {
    return QStringLiteral("the same model is on disk as %1; set %2 to it, or pass --use-on-disk")
        .arg(match.path, input.isEmpty() ? QStringLiteral("the input") : input);
}

std::optional<OnDiskMatch> findOnDisk(const QJsonObject &handle, const ModelLibrary &library, const RecommendContext &context, ModelShapes &shapes) {
    const QString resource = handle.value(QStringLiteral("resource")).toString();
    const QString wanted = handle.value(QStringLiteral("name")).toString();
    const std::optional<PortType> type = PortType::parse(resource);
    if (!type || wanted.isEmpty())
        return std::nullopt;
    const QString family = modelFamily(familyKindOf(resource), wanted);
    if (family.isEmpty())
        return std::nullopt;
    const QString stem = modelStem(wanted);
    // The wanted model's header figures, when a file of its name is here (another folder, another hash): then a renamed file of another
    // architecture is never offered (model_weights.h's rule). A model that is not on disk has no header to read, so its family and stem
    // are all there is to go by.
    QString wantedKey;
    for (const ModelFile &file : library.list(*type))
        if (QFileInfo(file.name).fileName().compare(QFileInfo(wanted).fileName(), Qt::CaseInsensitive) == 0) {
            wantedKey = weightsKey(shapes.of(file.path), file.name);
            if (!wantedKey.isEmpty())
                break;
        }
    // The same weights: one family, one stem, a header that reads and a quantization the engine can load.
    QList<ModelFile> same;
    for (const ModelFile &file : library.list(*type)) {
        if (modelFamily(familyKindOf(resource), file.name) != family || modelStem(file.name) != stem)
            continue;
        const std::optional<ModelShape> shape = shapes.of(file.path);
        if (!shape || !shape->unloadable.isEmpty())
            continue;
        if (!wantedKey.isEmpty() && weightsKey(shape, file.name) != wantedKey)
            continue;
        same << file;
    }
    if (same.isEmpty())
        return std::nullopt;
    const ModelFile *chosen = nullptr;
    bool recommended = false;
    if (same.size() > 1) {
        QList<MarkedFile> files;
        for (const ModelFile &file : std::as_const(same))
            files.append({file.path, file.size});
        const QHash<QString, ModelMark> marks = markModels(resource, files, context, shapes);
        for (const ModelFile &file : std::as_const(same))
            if (marks.value(file.path).recommended) {
                chosen = &file;
                recommended = true;
                break;
            }
        if (!chosen) {
            // Nothing is Recommended (no tier, or nothing fits): the best quality, then the smaller file.
            chosen = &*std::min_element(same.begin(), same.end(), [](const ModelFile &a, const ModelFile &b) {
                if (qualityTier(a.name) != qualityTier(b.name))
                    return qualityTier(a.name) > qualityTier(b.name);
                return a.size != b.size ? a.size < b.size : a.name < b.name;
            });
        }
    } else {
        chosen = &same.first();
    }
    OnDiskMatch match;
    match.name = chosen->name;
    match.path = chosen->path;
    match.resource = resource;
    match.tier = quantLabel(chosen->name);
    match.size = chosen->size;
    match.recommended = recommended;
    // The same quantization and not the same file (it is missing by hash): another source than the one the workflow names, whatever the words.
    match.otherSource = quantLabel(wanted) == quantLabel(chosen->name);
    return match;
}

QList<OnDiskUse> onDiskUses(const QList<MissingModel> &missing, const ModelLibrary &library, const RecommendContext &context, ModelShapes &shapes) {
    QList<OnDiskUse> out;
    for (const MissingModel &model : missing)
        if (const std::optional<OnDiskMatch> match = findOnDisk(model.handle, library, context, shapes))
            out.append({model.handle, *match});
    return out;
}

QJsonObject useHandle(const OnDiskUse &use, const QString &hash) {
    return QJsonObject{{"resource", use.match.resource}, {"hash", hash}, {"name", use.match.name}};
}

Uses resolveUses(const QList<OnDiskUse> &uses, FileHashes &hashes) {
    Uses out;
    for (const OnDiskUse &use : uses) {
        const QString hash = hashes.hash(use.match.path);
        if (!hash.isEmpty())
            out.insert(use.wanted.value(QStringLiteral("hash")).toString(), useHandle(use, hash));
    }
    return out;
}

QJsonValue withUses(const QJsonValue &value, const Uses &uses) {
    if (value.isArray()) {
        QJsonArray items;
        for (const QJsonValue &item : value.toArray())
            items.append(withUses(item, uses));
        return items;
    }
    if (!value.isObject())
        return value;
    QJsonObject object = value.toObject();
    if (object.value(QStringLiteral("resource")).isString()) {
        const auto found = uses.constFind(object.value(QStringLiteral("hash")).toString());
        if (found != uses.constEnd())
            return found.value();
    }
    for (auto it = object.begin(); it != object.end(); ++it)
        it.value() = withUses(it.value(), uses);
    return object;
}

namespace {

void replaceIn(Nodes &nodes, const Uses &uses) {
    for (Node &node : nodes)
        for (auto it = node.inputs.begin(); it != node.inputs.end(); ++it)
            if (it->kind() == InputValue::Kind::Resource || it->kind() == InputValue::Kind::List)
                it.value() = InputValue::fromJson(withUses(it->toJson(), uses));
}

} // namespace

Document withUses(const Document &document, const Uses &uses) {
    Document out = document;
    if (uses.isEmpty())
        return out;
    replaceIn(out.graph.nodes, uses);
    for (GroupDefinition &group : out.groups) {
        replaceIn(group.nodes, uses);
        for (GroupInput &input : group.inputs)
            if (input.defaultValue && (input.defaultValue->kind() == InputValue::Kind::Resource || input.defaultValue->kind() == InputValue::Kind::List))
                input.defaultValue = InputValue::fromJson(withUses(input.defaultValue->toJson(), uses));
    }
    return out;
}

} // namespace fugue
