// `fugue model list` (spec 13.1): the models found on disk (8.9) and offered by providers (8.2).
// `fugue model get <workflow|template>`: the models it names that are not here, downloaded (#89).
//
// A file on disk gets a handle only when its content hash is known (2.5: the hash is the
// identity), and listing reads no model file: hashing a model of several GB takes from seconds to
// minutes. `--hash` hashes the files not hashed yet, once (the hash is remembered for every Fugue
// process), so an agent gets a full handle for every file (13.4).

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>

#include <algorithm>
#include <optional>

#include "cli/commands.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/model_download.h"
#include "engine/model_library.h"
#include "engine/model_offers.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/templates.h"

namespace fugue::cli {

namespace {

QString sizeText(qint64 bytes) {
    return bytes >= 1000000000 ? QStringLiteral("%1 GB").arg(double(bytes) / 1e9, 0, 'f', 1)
                               : QStringLiteral("%1 MB").arg(std::max<qint64>(1, (bytes + 500000) / 1000000));
}

// The providers' models of `type` (every type when empty), waiting for every reply.
QList<ListedModel> offered(const QString &type) {
    QList<ListedModel> models;
    bool done = false;
    QEventLoop loop;
    listOfferedModels(type, {}, [&](QList<ListedModel> listed) {
        models = std::move(listed);
        done = true;
        loop.quit();
    });
    if (!done)
        loop.exec();
    return models;
}

} // namespace

Reply modelList(const Command &command, Context &context) {
    QList<const DeclaredPortType *> types;
    for (const DeclaredPortType *declared : context.registry.portTypes())
        if (declared->kind == PortKind::Resource)
            types << declared;
    if (command.has("type")) {
        const std::optional<PortType> wanted = PortType::parse(command.value("type"));
        const DeclaredPortType *declared = wanted ? context.registry.portType(*wanted) : nullptr;
        if (!declared || declared->kind != PortKind::Resource) {
            QJsonArray valid;
            for (const DeclaredPortType *each : types)
                valid.append(each->type.toString());
            Error error = makeError("usage", QStringLiteral("\"%1\" is not a resource type.").arg(command.value("type")),
                                    "Use one of the resource types the installed packs declare.");
            error.valid = valid;
            return Reply::failure(error);
        }
        types = {declared};
    }

    FileHashes hashes(FileHashes::defaultPath());
    const ModelLibrary library = ModelLibrary::current(&context.registry);
    QJsonArray models;
    QStringList lines;
    Errors warnings;
    for (const DeclaredPortType *declared : std::as_const(types)) {
        for (ListedModel model : listDiskModels(library, declared->type, hashes)) {
            if (!model.handle.contains("hash") && command.has("hash")) {
                // A file over 1 GB is told on stderr every tenth too, so a long read shows it is moving.
                context.log(QStringLiteral("Hashing %1 (%2)").arg(model.path, sizeText(model.size)));
                int shown = 0;
                const QString hash = hashes.hash(model.path, [&](qint64 done, qint64 total) {
                    const int tenth = total > 0 ? int(done * 10 / total) : 10;
                    if (tenth > shown && tenth < 10 && total > 1000000000)
                        context.log(QStringLiteral("  %1%").arg(tenth * 10));
                    shown = std::max(shown, tenth);
                    return true;
                });
                if (!hash.isEmpty()) {
                    model.handle = diskHandle(declared->type.toString(), model.name, hash);
                    hashes.save(); // after each file, so a stopped listing keeps what it did
                } else {
                    Error warning = makeError("media_unreadable",
                                              QStringLiteral("%1 could not be read, so it has no handle.").arg(model.path),
                                              "Check that the file is readable, then run `fugue model list --hash` again.");
                    warning.severity = Error::Severity::Warning;
                    warnings << warning;
                }
            }
            const bool hashed = model.handle.contains("hash");
            QJsonObject entry{{"source", "disk"},          {"type", declared->type.toString()},
                              {"name", model.name},        {"path", model.path},
                              {"folder", model.folder},    {"size", model.size},
                              {"hashed", hashed}};
            if (hashed)
                entry.insert("handle", model.handle);
            models.append(entry);
            lines << QStringLiteral("%1  %2  %3  %4")
                         .arg(declared->type.toString(), model.name,
                              hashed ? model.handle.value("hash").toString().left(19) + QStringLiteral("…")
                                     : QStringLiteral("(not hashed: --hash)"),
                              model.path);
        }
    }

    for (const ListedModel &model : offered(command.has("type") ? types.first()->type.toString() : QString())) {
        QJsonObject entry{{"source", "provider"},
                          {"type", model.handle.value("resource").toString()},
                          {"provider", model.provider},
                          {"name", model.name},
                          {"title", model.title.isEmpty() ? model.name : model.title},
                          {"status", model.status},
                          {"handle", model.handle}};
        if (!model.note.isEmpty())
            entry.insert("note", model.note);
        models.append(entry);
        lines << QStringLiteral("%1  %2  %3  %4")
                     .arg(model.handle.value("resource").toString(), model.name, model.status, model.provider);
    }
    Reply reply = Reply::success(QJsonObject{{"models", models}}, lines.join('\n'));
    reply.errors = warnings; // 13.2: a success with warnings
    return reply;
}

Reply modelGet(const Command &command, Context &context) {
    // A workflow file, else a template's id.
    const QString what = command.positionals.at(0);
    QJsonArray named;
    if (QFileInfo::exists(what)) {
        const Result<Document> document = Document::loadFile(what);
        if (!document)
            return Reply::failure(document.errors());
        named = document->models();
    } else {
        const TemplateLibrary templates(TemplateLibrary::standardRoots(), &context.registry);
        const std::optional<Template> chosen = templates.find(what);
        if (!chosen) {
            Error error = makeError("usage", QStringLiteral("There is no workflow file or template \"%1\".").arg(what),
                                    "Give a .fugue file, or the id of one `fugue template list` shows.");
            for (const Template &t : templates.list())
                error.valid.append(t.id);
            return Reply::failure(error);
        }
        named = chosen->models;
    }

    ModelLibrary library = ModelLibrary::current(&context.registry);
    if (command.has("into")) {
        const QString root = QDir::cleanPath(expandHome(command.value("into")));
        if (!library.roots().contains(root)) {
            Error error = makeError("usage", QStringLiteral("%1 is not a model folder.").arg(command.value("into")),
                                    "Use Fugue's own models folder or one of the extra model folders in Settings.");
            for (const QString &each : library.roots())
                error.valid.append(each);
            return Reply::failure(error);
        }
        library.setDownloadRoot(root);
    }

    FileHashes hashes(FileHashes::defaultPath());
    const QList<MissingModel> missing = library.missing(named, hashes);
    Errors errors;
    QList<DownloadTarget> targets;
    for (const MissingModel &model : missing) {
        const Result<DownloadTarget> target = planDownload(library, model);
        if (target)
            targets << target.value();
        else
            errors << target.errors();
    }

    QJsonArray downloaded;
    QStringList lines;
    if (!targets.isEmpty()) {
        QNetworkAccessManager network;
        DownloadQueue queue(&network, &hashes);
        QEventLoop loop;
        int shown = 0; // tenths of the file being fetched told on stderr
        QObject::connect(&queue, &DownloadQueue::started, &loop, [&](int index) {
            const DownloadTarget &target = queue.targets().at(index);
            shown = 0;
            context.log(QStringLiteral("Downloading %1%2 from %3 into %4")
                            .arg(target.handle.value("name").toString(),
                                 target.size >= 0 ? QStringLiteral(" (%1)").arg(sizeText(target.size)) : QString(),
                                 target.source.toString(), QFileInfo(target.path).absolutePath()));
        });
        // A file over 100 MB is told every tenth, so a long download shows it is moving.
        QObject::connect(&queue, &DownloadQueue::progress, &loop, [&](int, qint64 received, qint64 total) {
            const int tenth = total > 0 ? int(received * 10 / total) : 0;
            if (total > 100000000 && tenth > shown && tenth < 10)
                context.log(QStringLiteral("  %1% (%2 of %3)").arg(tenth * 10).arg(sizeText(received), sizeText(total)));
            shown = std::max(shown, tenth);
        });
        QObject::connect(&queue, &DownloadQueue::fileFinished, &loop, [&](int index, const Errors &failed) {
            const DownloadTarget &target = queue.targets().at(index);
            if (!failed.isEmpty()) {
                errors += failed;
                return;
            }
            downloaded.append(QJsonObject{{"name", target.handle.value("name")},
                                          {"type", target.handle.value("resource")},
                                          {"path", target.path},
                                          {"handle", target.handle}});
            lines << QStringLiteral("Downloaded %1 into %2.")
                         .arg(target.handle.value("name").toString(), QFileInfo(target.path).absolutePath());
        });
        QObject::connect(&queue, &DownloadQueue::finished, &loop, &QEventLoop::quit);
        queue.add(targets);
        loop.exec();
    }

    if (!errors.isEmpty()) {
        // What did arrive stays; the reply names what did not.
        for (const QString &line : std::as_const(lines))
            context.log(line);
        return Reply::failure(errors);
    }
    if (missing.isEmpty())
        lines << QStringLiteral("Every model it names is here.");
    return Reply::success(QJsonObject{{"downloaded", downloaded}, {"here", named.size() - qsizetype(missing.size())}},
                          lines.join('\n'));
}

} // namespace fugue::cli
