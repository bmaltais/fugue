#include "engine/model_weights.h"

#include <QFileInfo>
#include <QRegularExpression>

#include "engine/model_family.h"

namespace fugue {

QStringList modelNameWords(const QString &fileName) {
    QString name = QFileInfo(fileName).fileName().toLower();
    // Only a known extension is cut: "flux.2-klein" has a dot that is not one.
    for (const char *extension : {".safetensors", ".gguf", ".ckpt", ".pt", ".pth", ".bin"})
        if (name.endsWith(QLatin1String(extension))) {
            name.chop(qstrlen(extension));
            break;
        }
    static const QRegularExpression separators(QStringLiteral("[^a-z0-9]+"));
    return name.split(separators, Qt::SkipEmptyParts);
}

namespace {

bool isQuantWord(const QString &word) {
    static const QRegularExpression pattern(QStringLiteral(
        "^(i?q\\d+[a-z0-9]*|(bf|f|fp)(16|32)|fp[48]\\w*|nf4|int[48]|convrot|scaled|e\\dm\\d\\w*|mixed|w4a8|gptq|awq|svdq|nvfp4|gguf|safetensors|pruned)$"));
    return pattern.match(word).hasMatch();
}

// A word that goes with the quantization word before it (Q5_K_M: q5 k m; Q8_0: q8 0).
bool isQuantTail(const QString &word) {
    static const QRegularExpression pattern(QStringLiteral("^(k|s|m|l|xs|xl|0|1)$"));
    return pattern.match(word).hasMatch();
}

} // namespace

QString modelStem(const QString &fileName) {
    QString stem;
    bool afterQuant = false;
    for (const QString &word : modelNameWords(fileName)) {
        if (isQuantWord(word)) {
            afterQuant = word.startsWith(QLatin1Char('q')) || word.startsWith(QLatin1String("iq"));
            continue;
        }
        if (afterQuant && isQuantTail(word))
            continue;
        afterQuant = false;
        stem += word;
    }
    return stem;
}

QString weightsKey(const std::optional<ModelShape> &shape, const QString &fileName) {
    if (!shape || !shape->kind)
        return {};
    const QString family = modelFamily(modelKindName(*shape->kind), QFileInfo(fileName).fileName());
    if (family.isEmpty())
        return {};
    const auto number = [](const std::optional<int> &value) { return value ? QString::number(*value) : QStringLiteral("-"); };
    return family + QLatin1Char('|') + modelStem(fileName) + QLatin1Char('|') + number(shape->textWidth) + QLatin1Char(',') +
           number(shape->hidden) + QLatin1Char(',') + number(shape->latentChannels) + QLatin1Char(',') + number(shape->hiddenSize) +
           QLatin1Char(',') + shape->llmFamily;
}

} // namespace fugue
