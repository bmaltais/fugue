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
        {image, QStringLiteral("Z-Image"), re(QStringLiteral("z[-_]?image"))},
        {image, QStringLiteral("Qwen-Image"), re(QStringLiteral("qwen[-_]?image"))},
        {image, QStringLiteral("Wan 2.2"), re(QStringLiteral("wan[-_]?2[._]?2"))},
        {image, QStringLiteral("Wan 2.1"), re(QStringLiteral("wan[-_]?2[._]?1"))},
        {image, QStringLiteral("FLUX.1"), re(QStringLiteral("flux[-_.]?1"))},
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
        const QString family = modelFamily(model.value(QStringLiteral("resource")).toString(),
                                           model.value(QStringLiteral("name")).toString());
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
