#include "cli/canvas_run.h"
#include "cli/commands.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>

#include <unistd.h>

#include <memory>
#include <optional>

#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/model_catalog.h"
#include "engine/model_library.h"
#include "engine/pack_loader.h"
#include "engine/pack_manager.h"
#include "engine/pack_secrets.h"
#include "engine/readiness.h"
#include "engine/templates.h"
#include "engine/trust_store.h"

namespace fugue::cli {

namespace {

bool onTerminal() { return isatty(STDIN_FILENO) && isatty(STDERR_FILENO); }

void say(const QString &text) {
    QTextStream err(stderr);
    err << text << '\n';
    err.flush();
}

// What the person reads before answering.
void describe(const PackSummary &pack) {
    say(QStringLiteral("  Pack:        %1 %2 (%3)").arg(pack.name.isEmpty() ? pack.id : pack.name, pack.version, pack.id));
    if (!pack.oldVersion.isEmpty())
        say(QStringLiteral("  Replaces:    %1").arg(pack.oldVersion));
    if (!pack.source.location.isEmpty())
        say(QStringLiteral("  From:        %1%2").arg(pack.source.location,
                                                   pack.source.ref.isEmpty() ? QString() : QStringLiteral(" at %1 (%2)").arg(pack.source.ref, pack.source.commit.left(12))));
    say(QStringLiteral("  Runs as:     %1").arg(pack.runtimeKinds.isEmpty() ? QStringLiteral("(no nodes)") : pack.runtimeKinds.join(", ")));
    say(QStringLiteral("  Declares:    %1").arg(pack.permissions.isEmpty() ? QStringLiteral("nothing (network, files, GPU)") : pack.permissions.join(", ")));
    if (!pack.secrets.isEmpty()) {
        say(QStringLiteral("  Reads:       the secrets %1 (set under Settings > Packs, or `fugue pack secret set`)").arg(pack.secrets.join(", ")));
        if (!pack.secretsSet.isEmpty())
            say(QStringLiteral("  Already set: %1 (values are kept for this id from this source)").arg(pack.secretsSet.join(", ")));
        if (!pack.secretsOtherSource.isEmpty())
            say(QStringLiteral("  Warning:     %1 %2 already set for this pack id from ANOTHER source; this pack does not get %3.")
                    .arg(pack.secretsOtherSource.join(", "), pack.secretsOtherSource.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                         pack.secretsOtherSource.size() == 1 ? QStringLiteral("it") : QStringLiteral("them")));
        say(QStringLiteral("  Isolation:   advisory. A trusted pack's programs run as you and can read Fugue's config folder, where every key is kept."));
    }
    say(QStringLiteral("  Nodes:       %1").arg(pack.nodes.size()));
    if (!pack.setupCommand.isEmpty())
        say(QStringLiteral("  Setup:       %1").arg(displayCommand(pack.setupCommand)));
    say(QStringLiteral("  Fugue enforces no sandbox: a trusted pack runs programs as you."));
}

Answer yesNo(const QString &question) {
    if (!onTerminal())
        return Answer::NoHuman;
    QTextStream err(stderr);
    err << question << " [y/N] ";
    err.flush();
    static QTextStream in(stdin); // one reader: a second would lose what the first buffered
    const QString line = in.readLine().trimmed().toLower();
    if (line == QLatin1String("y") || line == QLatin1String("yes"))
        return Answer::Yes;
    return Answer::No;
}

Answer prompt(const PackSummary &pack, const QString &question, bool showPack = true) {
    if (!onTerminal())
        return Answer::NoHuman;
    if (showPack)
        describe(pack);
    return yesNo(question);
}

PackPrompts terminalPrompts(Context &context) {
    // Ctrl-C ends a git clone or a setup and cleans up after it, instead of killing the command in the middle of a change (#418).
    installStopHandlers();
    PackPrompts prompts;
    prompts.cancelled = [] { return stopRequested(); };
    prompts.install = [](const PackSummary &pack) {
        return prompt(pack, pack.oldVersion.isEmpty() ? QStringLiteral("Install this pack?") : QStringLiteral("Update to this version?"));
    };
    prompts.remove = [](const PackSummary &pack) { return prompt(pack, QStringLiteral("Remove this pack?")); };
    prompts.trust = [](const PackSummary &pack) { return prompt(pack, QStringLiteral("Trust this pack and let its nodes run?")); };
    prompts.runSetup = [](const PackSummary &pack) {
        if (!onTerminal())
            return Answer::NoHuman;
        say(QStringLiteral("  %1").arg(pack.setupDescription.isEmpty() ? QStringLiteral("The pack wants to run a setup command:") : pack.setupDescription));
        say(QStringLiteral("  In %1:").arg(pack.folder));
        say(QStringLiteral("    %1").arg(displayCommand(pack.setupCommand)));
        return yesNo(QStringLiteral("Run it?"));
    };
    prompts.log = [&context](const QString &line) {
        if (context.log)
            context.log(QStringLiteral("  ") + line);
    };
    return prompts;
}

QJsonObject summaryJson(const PackSummary &pack) {
    QJsonObject object{{"id", pack.id}, {"version", pack.version}, {"name", pack.name}, {"path", pack.folder},
                       {"runtime", QJsonArray::fromStringList(pack.runtimeKinds)},
                       {"permissions", QJsonArray::fromStringList(pack.permissions)},
                       {"secrets", QJsonArray::fromStringList(pack.secrets)}, // names only, never values
                       {"secrets_set", QJsonArray::fromStringList(pack.secretsSet)},
                       {"secrets_set_for_another_source", QJsonArray::fromStringList(pack.secretsOtherSource)},
                       {"nodes", QJsonArray::fromStringList(pack.nodes)}};
    if (!pack.setupCommand.isEmpty())
        object.insert("setup", QJsonObject{{"command", QJsonArray::fromStringList(pack.setupCommand)}, {"description", pack.setupDescription}});
    if (!pack.source.location.isEmpty()) {
        object.insert("source", pack.source.location);
        if (!pack.source.ref.isEmpty())
            object.insert("ref", pack.source.ref);
        if (!pack.source.commit.isEmpty())
            object.insert("commit", pack.source.commit);
    }
    return object;
}

QJsonObject addedJson(const PackAdded &added) {
    QJsonObject object = summaryJson(added.pack);
    object.insert("setup_ran", added.setupRan);
    object.insert("setup_skipped", added.setupSkipped);
    return object;
}

} // namespace

Reply packList(const Command &, Context &) {
    Registry scratch;
    const PackReport report = loadPacks(scratch, packSearchPaths(), inheritedPackPaths());
    const TrustStore trust;
    QJsonArray packs;
    QStringList lines;
    bool anyInherited = false;
    for (const LoadedPack &pack : report.packs) {
        QJsonObject object{{"id", pack.id}, {"version", pack.version}, {"name", pack.name}, {"path", pack.path}, {"enabled", pack.enabled},
                           {"trusted", pack.enabled && trust.isTrusted(pack.id, pack.version)},
                           {"origin", pack.inherited ? QStringLiteral("inherited from Default") : QStringLiteral("own")},
                           {"inherited", pack.inherited},
                           {"nodes", QJsonArray::fromStringList(pack.nodes)}};
        if (pack.inherited) {
            object.insert("hidden", pack.hidden);
            if (!pack.hidden) {
                object.insert("warning", inheritedPackWarning());
                anyInherited = true;
            }
        }
        const PackSource source = readPackSource(pack.path);
        if (!source.location.isEmpty()) {
            object.insert("source", source.location);
            if (!source.ref.isEmpty())
                object.insert("ref", source.ref);
        }
        if (!pack.errors.isEmpty())
            object.insert("errors", toJson(pack.errors));
        QJsonArray disabled;
        for (const PackProblem &problem : pack.disabled)
            disabled.append(QJsonObject{{"node", problem.node}, {"reason", problem.reason}});
        if (!disabled.isEmpty())
            object.insert("disabled_nodes", disabled);
        packs.append(object);
        const QString origin = pack.hidden ? QStringLiteral("  [inherited from Default, hidden by this project's own pack]")
                                           : pack.inherited ? QStringLiteral("  [inherited from Default]") : QString();
        if (pack.hidden) {
            lines << QStringLiteral("%1 %2%3").arg(pack.id, pack.version, origin);
            continue;
        }
        lines << (pack.enabled ? QStringLiteral("%1 %2  %3  %4 node(s)")
                                     .arg(pack.id, pack.version, trust.isTrusted(pack.id, pack.version) ? QStringLiteral("trusted") : QStringLiteral("untrusted"))
                                     .arg(pack.nodes.size())
                               : QStringLiteral("%1  disabled: %2").arg(pack.id.isEmpty() ? pack.path : pack.id,
                                                                         pack.errors.isEmpty() ? QStringLiteral("it did not load") : pack.errors.first().message))
              + origin;
    }
    if (anyInherited)
        lines << QString() << QStringLiteral("Warning: ") + inheritedPackWarning();
    Reply reply = Reply::success(QJsonObject{{"packs", packs}}, lines.isEmpty() ? QStringLiteral("No packs installed.") : lines.join('\n'));
    return reply;
}

Reply packAdd(const Command &command, Context &context) {
    TrustStore trust;
    // One question for the install, its setup command (shown in the same list) and the trust (#379).
    PackPrompts prompts = terminalPrompts(context);
    auto shown = std::make_shared<PackSummary>();
    prompts.install = [shown](const PackSummary &pack) {
        *shown = pack;
        return prompt(pack, QStringLiteral("Install this pack%1 and trust it?").arg(pack.setupCommand.isEmpty() ? QString() : QStringLiteral(", run its setup")));
    };
    prompts.runSetup = [](const PackSummary &) { return Answer::Yes; }; // shown above, covered by that yes
    // Trusted only if, after the setup, it is still the pack that was shown.
    prompts.trust = [shown](const PackSummary &now) { return samePack(*shown, now) ? Answer::Yes : Answer::No; };
    const Result<PackAdded> added = addPack(command.positionals.at(0), command.value("ref"), packsDir(), trust, prompts);
    if (!added)
        return Reply::failure(added.errors());
    QString text = QStringLiteral("Installed %1 %2.").arg(added->pack.id, added->pack.version);
    if (added->setupRan)
        text += QStringLiteral(" Its setup ran.");
    // Beside another major of the pack its folder is <id>@<major>, which the pack commands take.
    const Result<PackSummary> trusted = trustPack(QFileInfo(added->pack.folder).fileName(), packsDir(), trust, prompts);
    if (!trusted)
        return Reply::failure(trusted.errors().first().code == QLatin1String("cancelled")
                                  ? Errors{makeError(QStringLiteral("pack_invalid"), QStringLiteral("The pack changed while it was set up, so it was installed but not trusted."),
                                                     QStringLiteral("Look at it, then `fugue pack trust %1`.").arg(added->pack.id))}
                                  : trusted.errors());
    text += QStringLiteral(" Trusted it.");
    Reply reply = Reply::success(addedJson(*added), text);
    reply.errors = added.warnings();
    return reply;
}

namespace {

// The stores a readiness plan reads (#379), kept alive for as long as the plan's env points at them.
struct ReadinessStores {
    FileHashes hashes{FileHashes::defaultPath()};
    TrustStore trust;
    const ModelCatalog catalog = ModelCatalog::load();

    ReadinessEnv envFor(Context &context) {
        ReadinessEnv env{ModelLibrary::current(&context.registry)};
        env.registry = &context.registry;
        env.hashes = &hashes;
        env.trust = &trust;
        env.packsDir = packsDir();
        env.catalog = &catalog;
        return env;
    }
};

} // namespace

ReadinessPlan readinessPlanFor(const ReadinessRequest &request, Context &context) {
    ReadinessStores stores;
    return buildReadinessPlan(request, stores.envFor(context));
}

namespace {

// Shows the plan, asks once on a terminal, runs it (#379). With no terminal it is needs_confirmation.
Reply prepare(const ReadinessRequest &request, Context &context, const QString &what) {
    ReadinessStores stores;
    FileHashes &hashes = stores.hashes;
    TrustStore &trust = stores.trust;
    ReadinessEnv env = stores.envFor(context);
    installStopHandlers(); // Ctrl-C stops the run between steps and ends a setup cleanly (#418)
    env.prompts.cancelled = [] { return stopRequested(); };
    env.prompts.log = [&context](const QString &line) { context.log(QStringLiteral("  ") + line); };
    const ReadinessPlan plan = buildReadinessPlan(request, env); // reads folders only: no network before the yes
    // Problems are not "ready": what cannot be done is said and the exit is non-zero.
    const auto problemErrors = [&plan] {
        Errors errors;
        for (Error problem : plan.problems) {
            problem.severity = Error::Severity::Error;
            errors.append(problem);
        }
        return errors;
    };
    if (plan.empty()) {
        if (!plan.problems.isEmpty())
            return Reply::failure(problemErrors());
        return Reply::success(QJsonObject{{"plan", plan.toJson()}, {"ready", true}}, QStringLiteral("%1 is ready.").arg(what));
    }
    if (!onTerminal()) {
        Error error = makeError(QStringLiteral("needs_confirmation"),
                                QStringLiteral("Getting %1 ready needs a person to agree, and there is no terminal to ask on.").arg(what),
                                QStringLiteral("The list is: %1 Run the same command in a terminal; no flag answers for the person.").arg(plan.headline()));
        return Reply::failure(Errors{error} + problemErrors());
    }
    for (const QString &line : plan.lines())
        say(line);
    if (plan.lowSpace())
        return Reply::failure(makeError(QStringLiteral("download_failed"), QStringLiteral("There is not enough free space where the models go (%1).").arg(plan.destination),
                                        QStringLiteral("Change Settings > Folders > Download models into, then run it again.")));
    if (yesNo(QStringLiteral("Install and trust all?")) != Answer::Yes)
        return Reply::failure(makeError(QStringLiteral("cancelled"), QStringLiteral("Nothing was changed.")));
    ReadinessRun run{packsDir(), &trust, &hashes};
    run.log = env.prompts.log;
    run.cancelled = [] { return stopRequested(); };
    run.allowMore = [](const PackSummary &pack, const QStringList &permissions) {
        say(QStringLiteral("  %1 also wants %2 access.").arg(pack.name.isEmpty() ? pack.id : pack.name, permissions.join(QStringLiteral(", "))));
        return yesNo(QStringLiteral("Allow it? (no skips this pack)")) == Answer::Yes;
    };
    run.onRow = [&plan](int row, RowState state, const QString &detail) {
        if (state != RowState::Done && state != RowState::Failed)
            return;
        const QString name = row < plan.packs.size() ? plan.packs.at(row).pack.id : plan.models.at(row - plan.packs.size()).name;
        say(QStringLiteral("  %1 %2%3").arg(state == RowState::Done ? QStringLiteral("done  ") : QStringLiteral("FAILED"), name,
                                         detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
    };
    const ReadinessOutcome outcome = runReadinessPlan(plan, run);
    if (!outcome.ok) {
        Errors errors = outcome.errors;
        if (errors.isEmpty() && !outcome.cancelled)
            errors << makeError(QStringLiteral("cancelled"), QStringLiteral("Some rows were skipped."));
        if (errors.isEmpty())
            errors << makeError(QStringLiteral("cancelled"), QStringLiteral("Stopped."));
        errors.first().hint = QStringLiteral("Not done: %1. Run the same command again; it lists only what is still missing.").arg(outcome.retry.join(QStringLiteral(", ")));
        return Reply::failure(errors);
    }
    if (!plan.problems.isEmpty())
        return Reply::failure(problemErrors());
    return Reply::success(QJsonObject{{"plan", plan.toJson()}, {"ready", true}, {"done", outcome.done}},
                          QStringLiteral("%1 is ready.").arg(what));
}

} // namespace

Reply templatePrepare(const Command &command, Context &context) {
    const TemplateLibrary library(TemplateLibrary::standardRoots(), &context.registry);
    const std::optional<Template> chosen = library.find(command.positionals.at(0));
    if (!chosen) {
        Error error = makeError("usage", QStringLiteral("There is no template \"%1\".").arg(command.positionals.at(0)),
                                "Use the id of one `fugue template list` shows.");
        for (const Template &t : library.list())
            error.valid.append(t.id);
        return Reply::failure(error);
    }
    const Result<Document> document = Document::loadFile(chosen->workflow);
    if (!document)
        return Reply::failure(document.errors());
    return prepare(requestFor(*document, context.registry, chosen->carriedPacks, chosen->folder), context, chosen->title);
}

Reply workflowPrepare(const Command &command, Context &context) {
    const Result<Document> document = Document::loadFile(command.positionals.at(0));
    if (!document)
        return Reply::failure(document.errors());
    return prepare(requestFor(*document, context.registry, {}, QFileInfo(command.positionals.at(0)).absolutePath()), context, QFileInfo(command.positionals.at(0)).fileName());
}

Reply packRemove(const Command &command, Context &context) {
    TrustStore trust;
    const Result<PackSummary> removed = removePack(command.positionals.at(0), packsDir(), trust, terminalPrompts(context));
    if (!removed)
        return Reply::failure(removed.errors());
    return Reply::success(QJsonObject{{"removed", removed->id}}, QStringLiteral("Removed %1.").arg(removed->id));
}

Reply packUpdate(const Command &command, Context &context) {
    TrustStore trust;
    const Result<QList<PackAdded>> changed = updatePacks(command.positionals.value(0), command.value("ref"), packsDir(), trust, terminalPrompts(context));
    if (!changed)
        return Reply::failure(changed.errors());
    QJsonArray updated;
    QStringList lines;
    for (const PackAdded &added : *changed) {
        updated.append(addedJson(added));
        lines << QStringLiteral("Updated %1 to %2.").arg(added.pack.id, added.pack.version);
        if (added.setupSkipped)
            lines << QStringLiteral("  Its setup was not run; run it yourself in %1.").arg(added.pack.folder);
    }
    Reply reply = Reply::success(QJsonObject{{"updated", updated}}, lines.isEmpty() ? QStringLiteral("Nothing to update.") : lines.join('\n'));
    reply.errors = changed.warnings(); // packs that could not be fetched
    return reply;
}

Reply packTrust(const Command &command, Context &context) {
    TrustStore trust;
    const Result<PackSummary> done = trustPack(command.positionals.at(0), packsDir(), trust, terminalPrompts(context));
    if (!done)
        return Reply::failure(done.errors());
    return Reply::success(QJsonObject{{"trusted", done->id}, {"version", done->version}},
                          QStringLiteral("Trusted %1 %2.").arg(done->id, done->version));
}

namespace {

// The enabled pack `id` (or `<id>@<major>`) of the packs this session finds, with the secrets it declares.
std::optional<LoadedPack> packDeclaring(const QString &id) {
    Registry scratch;
    const PackRef ref = parsePackRef(id);
    const PackReport report = loadPacks(scratch, packSearchPaths(), inheritedPackPaths());
    for (const LoadedPack &pack : report.packs)
        if (pack.enabled && !pack.hidden && pack.id == ref.id)
            return pack;
    return std::nullopt;
}

// The store key of a loaded pack: its id and where it came from (6.4.1).
QString keyOf(const LoadedPack &pack) { return packSecretKey(pack.id, readPackSource(pack.path).location); }

QJsonObject secretsJson(const LoadedPack &pack, const PackSecretStore &store) {
    const QString key = keyOf(pack);
    const QStringList set = store.namesSet(key);
    QJsonArray list;
    for (const PackSecretSpec &spec : pack.secrets) {
        QJsonObject entry{{"name", spec.name}, {"label", spec.label}, {"set", set.contains(spec.name)}, {"origin_required", spec.originRequired}};
        if (set.contains(spec.name))
            entry.insert("origin", store.originOf(key, spec.name)); // where it may be sent; never the value
        list.append(entry);
    }
    return QJsonObject{{"id", pack.id}, {"version", pack.version}, {"secrets", list}};
}

} // namespace

Reply packSecret(const Command &command, Context &) {
    const QString action = command.positionals.at(0);
    PackSecretStore store;
    const QString packId = command.positionals.value(1);
    const QString name = command.positionals.value(2);
    if (action == QLatin1String("list")) {
        if (command.positionals.size() > 2)
            return Reply::failure(makeError(QStringLiteral("usage"), QStringLiteral("`pack secret list` takes a pack id at most.")));
        Registry scratch;
        const PackReport report = loadPacks(scratch, packSearchPaths(), inheritedPackPaths());
        QJsonArray packs;
        QStringList lines;
        for (const LoadedPack &pack : report.packs) {
            if (!pack.enabled || pack.hidden || pack.secrets.isEmpty() || (!packId.isEmpty() && parsePackRef(packId).id != pack.id))
                continue;
            packs.append(secretsJson(pack, store));
            const QStringList set = store.namesSet(keyOf(pack));
            for (const PackSecretSpec &spec : pack.secrets) {
                const QString origin = store.originOf(keyOf(pack), spec.name);
                lines << QStringLiteral("%1  %2  %3%4").arg(pack.id, spec.name, set.contains(spec.name) ? QStringLiteral("set") : QStringLiteral("not set"),
                                                              origin.isEmpty() ? QString() : QStringLiteral("  only for %1").arg(origin));
            }
        }
        return Reply::success(QJsonObject{{"packs", packs}},
                              lines.isEmpty() ? QStringLiteral("No installed pack declares a secret.") : lines.join('\n'));
    }
    if ((action != QLatin1String("set") && action != QLatin1String("clear")) || packId.isEmpty() || name.isEmpty() || command.positionals.size() != 3)
        return Reply::failure(makeError(QStringLiteral("usage"), QStringLiteral("Use `pack secret list [<pack>]`, `pack secret set <pack> <name>` or `pack secret clear <pack> <name>`."),
                                        QStringLiteral("`set` reads the value from stdin, so it is never on a command line or in a shell history.")));
    const std::optional<LoadedPack> pack = packDeclaring(packId);
    if (!pack)
        return Reply::failure(makeError(QStringLiteral("unknown_pack"), QStringLiteral("No pack %1 is installed and loaded.").arg(packId),
                                        QStringLiteral("`fugue pack list` shows the packs.")));
    if (!packSecretNames(pack->secrets).contains(name)) {
        Error error = makeError(QStringLiteral("usage"), QStringLiteral("%1 declares no secret named \"%2\".").arg(pack->id, name.left(40)));
        for (const QString &known : packSecretNames(pack->secrets))
            error.valid.append(known);
        return Reply::failure(error);
    }
    if (action == QLatin1String("clear")) {
        const Errors cleared = store.clear(keyOf(*pack), name);
        if (hasErrors(cleared))
            return Reply::failure(cleared);
        return Reply::success(secretsJson(*pack, store), QStringLiteral("Cleared %1 of %2.").arg(name, pack->id));
    }
    if (isatty(STDIN_FILENO))
        return Reply::failure(makeError(QStringLiteral("usage"), QStringLiteral("The value is read from stdin, and stdin is a terminal."),
                                        QStringLiteral("Pipe it in: `read -rs v; printf %s \"$v\" | fugue pack secret set %1 %2`.").arg(pack->id, name)));
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly))
        return Reply::failure(makeError(QStringLiteral("usage"), QStringLiteral("Cannot read the value from stdin.")));
    QByteArray bytes = in.read(4097);
    if (bytes.size() > 4096)
        return Reply::failure(makeError(QStringLiteral("usage"), QStringLiteral("A secret is at most 4096 bytes.")));
    if (bytes.endsWith('\n'))
        bytes.chop(1);
    if (bytes.endsWith('\r'))
        bytes.chop(1);
    const QString value = QString::fromUtf8(bytes);
    const bool required = [&] {
        for (const PackSecretSpec &spec : pack->secrets)
            if (spec.name == name)
                return spec.originRequired;
        return false;
    }();
    const Errors stored = store.set(keyOf(*pack), name, value, command.value("origin"), required);
    if (hasErrors(stored))
        return Reply::failure(stored);
    return Reply::success(secretsJson(*pack, store), QStringLiteral("Stored %1 of %2.").arg(name, pack->id));
}

} // namespace fugue::cli
