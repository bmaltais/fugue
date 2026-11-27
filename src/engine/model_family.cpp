#include "engine/model_family.h"

#include <QJsonObject>
#include <QList>
#include <QRegularExpression>

#include <utility>

namespace fugue {

namespace {

struct Rule {
    QStringList kinds; // the resource types it applies to (the part after the "/")
    QString name;
    QRegularExpression pattern; // against the file name, case-insensitive
};

QRegularExpression re(const QString &pattern) {
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption);
}

// First match wins, so a more specific rule goes above a general one.
const QList<Rule> &rules() {
    static const QStringList image{QStringLiteral("diffusion-model"), QStringLiteral("checkpoint")};
    static const QList<Rule> table{
        {image, QStringLiteral("FLUX.2 Klein 9B"), re(QStringLiteral("flux[-_.]?2.*klein.*9b"))},
        {image, QStringLiteral("FLUX.2 Klein 4B"), re(QStringLiteral("flux[-_.]?2.*klein.*4b"))},
        // Krea 2 (#684), Turbo and Raw: one architecture, so one family; the files are told apart by their stem.
        {image, QStringLiteral("Krea 2"), re(QStringLiteral("krea[-_ ]?2"))},
        {image, QStringLiteral("Z-Image"), re(QStringLiteral("z[-_]?image"))},
        {image, QStringLiteral("Qwen-Image"), re(QStringLiteral("qwen[-_]?image"))},
        {image, QStringLiteral("Wan 2.2"), re(QStringLiteral("wan[-_]?2[._]?2"))},
        {image, QStringLiteral("Wan 2.1"), re(QStringLiteral("wan[-_]?2[._]?1"))},
        {image, QStringLiteral("FLUX.1"), re(QStringLiteral("flux[-_.]?1"))},
        // Text encoders (#370): the LLM a diffusion model reads. #369's templates name none of these.
        {{QStringLiteral("text-encoder")}, QStringLiteral("Qwen3.5 9B"), re(QStringLiteral("qwen[-_]?3[._]?5.*9b"))},
        {{QStringLiteral("text-encoder")}, QStringLiteral("Qwen3 8B"), re(QStringLiteral("qwen[-_]?3.*8b"))},
        // Krea 2 reads Qwen3-VL 4B (#684): above Qwen3 4B, whose pattern also matches qwen3vl_4b_*.
        {{QStringLiteral("text-encoder")}, QStringLiteral("Qwen3-VL 4B"), re(QStringLiteral("qwen[-_]?3[-_]?vl.*4b"))},
        {{QStringLiteral("text-encoder")}, QStringLiteral("Qwen3 4B"), re(QStringLiteral("qwen[-_]?3.*4b"))},
        {{QStringLiteral("seedvr2-model")}, QStringLiteral("SeedVR2"), re(QStringLiteral(""))},
        {{QStringLiteral("upscale-model")}, QStringLiteral("Real-ESRGAN"), re(QStringLiteral("esrgan"))},
        {{QStringLiteral("birefnet-model")}, QStringLiteral("BiRefNet"), re(QStringLiteral(""))},
    };
    return table;
}

} // namespace

QString modelFamily(const QString &resource, const QString &name) {
    const QString kind = resource.section(QLatin1Char('/'), -1);
    for (const Rule &rule : rules())
        if (rule.kinds.contains(kind) && rule.pattern.match(name).hasMatch())
            return rule.name;
    return {};
}

QStringList modelFamilies(const QJsonArray &models) {
    QStringList out;
    for (const QJsonValue value : models) {
        const QJsonObject model = value.toObject();
        const QString resource = model.value(QStringLiteral("resource")).toString();
        // A template's families are its main models: a text encoder serves many (Qwen3 8B reads for Klein 9B and
        // others), so it is a picker family (#370) but never a template's (#369).
        if (resource.endsWith(QLatin1String("text-encoder")))
            continue;
        const QString family = modelFamily(resource, model.value(QStringLiteral("name")).toString());
        if (!family.isEmpty() && !out.contains(family))
            out << family;
    }
    return out;
}

QStringList knownModelFamilies() {
    QStringList out;
    for (const Rule &rule : rules())
        if (!out.contains(rule.name))
            out << rule.name;
    return out;
}

} // namespace fugue
