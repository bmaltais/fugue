#include "cli/use_on_disk.h"

#include "engine/file_hashes.h"
#include "engine/lazy_reach.h"
#include "engine/model_library.h"
#include "engine/model_use.h"

namespace fugue::cli {

namespace {

// "node.input" of the first input that holds a handle with `hash` (a derived handle nests its parts).
bool holds(const QJsonValue &value, const QString &hash) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            if (holds(item, hash))
                return true;
        return false;
    }
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    if (object.value(QStringLiteral("hash")).toString() == hash)
        return true;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (holds(it.value(), hash))
            return true;
    return false;
}

QString inputOf(const Document &document, const QString &hash) {
    for (auto node = document.graph.nodes.cbegin(); node != document.graph.nodes.cend(); ++node)
        for (auto input = node->inputs.cbegin(); input != node->inputs.cend(); ++input)
            if (holds(input->toJson(), hash))
                return node.key() + QLatin1Char('.') + input.key();
    return {};
}

QList<OnDiskUse> usesFor(const Document &document, Context &context) {
    FileHashes hashes(FileHashes::defaultPath());
    const ModelLibrary library = ModelLibrary::current(&context.registry);
    const QList<MissingModel> missing = library.missing(modelsToHave(document, context.registry), hashes);
    if (missing.isEmpty())
        return {}; // a plain run with everything here reads no folder, no header and no device
    const ModelSpeeds speeds;
    return onDiskUses(missing, library, RecommendContext{localComputeDevice(), 0, &speeds}, ModelShapes::shared());
}

} // namespace

Document useOnDisk(const Document &document, Context &context, QStringList *said) {
    const QList<OnDiskUse> uses = usesFor(document, context);
    if (uses.isEmpty())
        return document;
    FileHashes hashes(FileHashes::defaultPath());
    const Uses resolved = resolveUses(uses, hashes);
    hashes.save(); // a file read once is known to every process
    for (const OnDiskUse &use : uses) {
        if (!resolved.contains(use.wanted.value(QStringLiteral("hash")).toString()))
            continue;
        QString line = QStringLiteral("--use-on-disk: %1 -> %2 (same model, %3)")
                           .arg(use.wanted.value(QStringLiteral("name")).toString(), use.match.path, use.match.tier.isEmpty() ? QStringLiteral("same quantization") : use.match.tier);
        if (use.match.otherSource)
            line += QStringLiteral("; another source than the one the workflow names, not verified to be identical");
        if (said)
            *said << line;
        if (context.log)
            context.log(line);
    }
    return withUses(document, resolved);
}

Errors onDiskNotes(const Document &document, Context &context) {
    Errors notes;
    for (const OnDiskUse &use : usesFor(document, context)) {
        const QString input = inputOf(document, use.wanted.value(QStringLiteral("hash")).toString());
        Error note = makeError(QStringLiteral("model_missing"),
                               QStringLiteral("The model %1 is not on this machine.").arg(use.wanted.value(QStringLiteral("name")).toString()),
                               use.cliHint(input));
        note.severity = Error::Severity::Warning;
        note.node = input.section(QLatin1Char('.'), 0, 0);
        note.input = input.section(QLatin1Char('.'), 1);
        notes.append(note);
    }
    return notes;
}

void addOnDiskHints(Errors &errors, const Document &document, Context &context) {
    bool any = false;
    for (const Error &error : std::as_const(errors))
        any |= error.code == QLatin1String("model_missing") || error.code == QLatin1String("app_not_ready");
    if (!any)
        return;
    const Errors notes = onDiskNotes(document, context);
    for (Error &error : errors) {
        if (error.code == QLatin1String("model_missing") && !error.node.isEmpty()) {
            for (const Error &note : notes)
                if (note.node == error.node && note.input == error.input)
                    error.hint = (error.hint.isEmpty() ? QString() : error.hint + QLatin1Char(' ')) + note.hint + QLatin1Char('.');
        } else if (error.code == QLatin1String("app_not_ready")) {
            for (const Error &note : notes)
                error.hint = (error.hint.isEmpty() ? QString() : error.hint + QLatin1Char(' ')) + note.hint + QLatin1Char('.');
        }
    }
}

} // namespace fugue::cli
