// `fugue model list` (spec 13.1): the models found on disk (8.9) and offered by providers (8.2).
//
// A file on disk gets a handle only when its content hash is known (2.5: the hash is the
// identity), and listing reads no model file: hashing a model of several GB takes from seconds to
// minutes. `--hash` hashes the files not hashed yet, once (the hash is remembered for every Fugue
// process), so an agent gets a full handle for every file (13.4).

#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <optional>

#include "cli/commands.h"
#include "engine/file_hashes.h"
#include "engine/model_library.h"
#include "engine/model_offers.h"
#include "engine/registry.h"

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

} // namespace fugue::cli
