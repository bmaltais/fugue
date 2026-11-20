#include "cli/commands.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>

#include <unistd.h>

#include "engine/pack_loader.h"
#include "engine/pack_manager.h"
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
    say(QStringLiteral("  Nodes:       %1").arg(pack.nodes.size()));
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
    PackPrompts prompts;
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
    const PackReport report = loadPacks(scratch, packSearchPaths());
    const TrustStore trust;
    QJsonArray packs;
    QStringList lines;
    for (const LoadedPack &pack : report.packs) {
        QJsonObject object{{"id", pack.id}, {"version", pack.version}, {"name", pack.name}, {"path", pack.path}, {"enabled", pack.enabled},
                           {"trusted", pack.enabled && trust.isTrusted(pack.id, pack.version)},
                           {"nodes", QJsonArray::fromStringList(pack.nodes)}};
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
        lines << (pack.enabled ? QStringLiteral("%1 %2  %3  %4 node(s)")
                                     .arg(pack.id, pack.version, trust.isTrusted(pack.id, pack.version) ? QStringLiteral("trusted") : QStringLiteral("untrusted"))
                                     .arg(pack.nodes.size())
                               : QStringLiteral("%1  disabled: %2").arg(pack.id.isEmpty() ? pack.path : pack.id,
                                                                         pack.errors.isEmpty() ? QStringLiteral("it did not load") : pack.errors.first().message));
    }
    Reply reply = Reply::success(QJsonObject{{"packs", packs}}, lines.isEmpty() ? QStringLiteral("No packs installed.") : lines.join('\n'));
    return reply;
}

Reply packAdd(const Command &command, Context &context) {
    TrustStore trust;
    const Result<PackAdded> added = addPack(command.positionals.at(0), command.value("ref"), packsDir(), trust, terminalPrompts(context));
    if (!added)
        return Reply::failure(added.errors());
    QString text = QStringLiteral("Installed %1 %2.").arg(added->pack.id, added->pack.version);
    if (added->setupSkipped)
        text += QStringLiteral(" Its setup was not run; run it yourself in %1.").arg(added->pack.folder);
    text += QStringLiteral(" Trust it with `fugue pack trust %1` before it runs.").arg(added->pack.id);
    Reply reply = Reply::success(addedJson(*added), text);
    reply.errors = added.warnings();
    return reply;
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

} // namespace fugue::cli
