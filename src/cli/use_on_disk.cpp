#include "cli/use_on_disk.h"

#include <QElapsedTimer>
#include <QJsonObject>

#include "engine/json.h"

#include "engine/file_hashes.h"
#include "engine/lazy_reach.h"
#include "engine/model_library.h"
#include "engine/model_use.h"
#include "engine/settings.h"

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

// What a command that waits says, so it is never silent (#625): a line on stderr, or with --json the event
// {"event":"waiting","for":...} on stderr too (stdout stays the one envelope).
void sayWaiting(Context &context, const QString &what, const QJsonObject &fields, const QString &text) {
    if (!context.log)
        return;
    QJsonObject event{{"event", "waiting"}, {"for", what}};
    for (auto it = fields.begin(); it != fields.end(); ++it)
        event.insert(it.key(), it.value());
    context.log(context.json ? QString::fromUtf8(compactJson(event)) : text);
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

bool usesOnDisk(const Command &command) {
    if (command.has("no-use-on-disk"))
        return false;
    return command.has("use-on-disk") || currentSettings().useOnDisk == QLatin1String("use");
}

Errors onDiskFlagConflict(const Command &command) {
    if (command.has("use-on-disk") && command.has("no-use-on-disk"))
        return {makeError("usage", QStringLiteral("--use-on-disk and --no-use-on-disk cannot be used together."), QStringLiteral("Pass one of them."))};
    return {};
}

Document useOnDisk(const Document &document, Context &context, QStringList *said) {
    const QList<OnDiskUse> uses = usesFor(document, context);
    if (uses.isEmpty())
        return document;
    FileHashes hashes(FileHashes::defaultPath());
    // A file never hashed is read whole (gigabytes): said once it has taken a second.
    QElapsedTimer reading;
    QString readingPath;
    bool told = false;
    const Uses resolved = resolveUses(uses, hashes, [&](const QString &path, qint64, qint64) {
        if (path != readingPath) {
            readingPath = path;
            told = false;
            reading.start();
        }
        if (!told && reading.elapsed() >= 1000) {
            told = true;
            sayWaiting(context, QStringLiteral("model_hashes"), QJsonObject{{"file", path}},
                       QStringLiteral("Reading %1 to tell it is the model (once; the hash is remembered)…").arg(path));
        }
    });
    // A file read once is known to every process. Another Fugue may be writing the store: said after a second, and given up
    // after ten, with the model used all the same (the hash is only a memo).
    qint64 holder = 0;
    bool waited = false;
    if (!hashes.save([&](qint64 pid) {
            waited = true;
            holder = pid;
            sayWaiting(context, QStringLiteral("model_hash_cache"), pid > 0 ? QJsonObject{{"pid", double(pid)}} : QJsonObject(),
                       pid > 0 ? QStringLiteral("Waiting for another Fugue (pid %1) to finish reading model hashes…").arg(pid)
                               : QStringLiteral("Waiting for another Fugue to finish reading model hashes…"));
        }) && waited && context.log) {
        context.log(QStringLiteral("warning: the model hashes were not saved: another Fugue%1 held the hash cache for 10 seconds. The check went on without saving; the next run reads them again.")
                        .arg(holder > 0 ? QStringLiteral(" (pid %1)").arg(holder) : QString()));
    }
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
