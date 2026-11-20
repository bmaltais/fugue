#include "engine/group_library.h"

#include "engine/atomic_file.h"
#include "engine/groups.h"
#include "engine/ids.h"
#include "engine/json.h"
#include "engine/links.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace fugue {

namespace {

constexpr const char *kDefinition = "group.fugue";

Error libraryError(const QString &message, const QString &hint = {}) {
    Error error;
    error.code = QStringLiteral("group_invalid");
    error.message = message;
    error.hint = hint;
    return error;
}

// The folders pack.json lists.
QStringList libraryFolders(const QString &dir) {
    QStringList nodes;
    if (const Result<QJsonObject> old = readJsonObject(dir + QStringLiteral("/pack.json")); old.ok())
        for (const QJsonValue &node : old->value("nodes").toArray())
            nodes << node.toString();
    return nodes;
}

// Writes the library's pack.json, listing `folders`.
Errors writeLibraryPack(const QString &dir, QStringList folders) {
    QJsonObject pack{{"manifest", 1},
                     {"id", "local"},
                     {"version", "1.0.0"},
                     {"name", "Library"},
                     {"description", "Groups saved from workflows."}};
    folders.removeDuplicates();
    folders.sort();
    pack.insert("nodes", QJsonArray::fromStringList(folders));
    return writeJsonFile(dir + QStringLiteral("/pack.json"), pack);
}

// The folder of a library group's id, local/<name>; empty when it is not one of the library's.
QString libraryFolder(const QString &id, const QString &dir) {
    const QString name = id.section(QLatin1Char('/'), 1);
    if (!id.startsWith(QLatin1String("local/")) || name.isEmpty() || !libraryFolders(dir).contains(name))
        return {};
    return dir + QLatin1Char('/') + name;
}

// A definition as a file writes it, to tell two apart.
QJsonValue definitionJson(const GroupDefinition &group) {
    Document holder;
    holder.groups.insert(QStringLiteral("g"), group);
    holder.present.groups = true;
    return holder.toJson().value("groups").toObject().value("g");
}

// `group` and every group it uses, through the document's definitions.
QStringList usedGroups(const Document &document, const QString &group) {
    QStringList found, todo{group};
    while (!todo.isEmpty()) {
        const QString id = todo.takeFirst();
        const GroupDefinition *definition = document.group(id);
        if (!definition || found.contains(id))
            continue;
        found << id;
        for (const Node &node : definition->nodes)
            if (document.group(node.type))
                todo << node.type;
    }
    return found;
}

// The input and output names of a face, each with its type, to compare two versions (3.6).
QHash<QString, QString> ports(const QList<InputSpec> &inputs) {
    QHash<QString, QString> found;
    for (const InputSpec &input : inputs)
        found.insert(input.name, input.type.toString());
    return found;
}

QHash<QString, QString> ports(const QList<OutputSpec> &outputs) {
    QHash<QString, QString> found;
    for (const OutputSpec &output : outputs)
        found.insert(output.name, output.type.toString());
    return found;
}

// Whether `after` lacks or retypes a port of `before`; whether it adds one.
void compare(const QHash<QString, QString> &before, const QHash<QString, QString> &after, bool *broke, bool *grew) {
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!after.contains(it.key()) || after.value(it.key()) != it.value())
            *broke = true;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (!before.contains(it.key()))
            *grew = true;
}

} // namespace

QString libraryDir() { return packsDir() + QStringLiteral("/local"); }

Result<SavedGroup> saveToLibrary(const Document &document, const QString &group, const Registry &registry,
                                 const QString &dir) {
    const GroupDefinition *definition = document.group(group);
    if (!definition)
        return libraryError(QStringLiteral("There is no group %1.").arg(group));
    const QString name = group.section(QLatin1Char('/'), 1);
    const QString id = QStringLiteral("local/") + name;
    if (!isTypeId(id))
        return libraryError(QStringLiteral("%1 cannot be saved to the library under that name.").arg(group),
                            QStringLiteral("Rename the group first."));

    // The file: the group (under its library id) and the groups it uses, with their layout.
    Document file;
    file.present.groups = true;
    const QJsonObject layouts = document.layout.value("groups").toObject();
    QJsonObject fileLayouts;
    for (const QString &used : usedGroups(document, group)) {
        GroupDefinition copy = *document.group(used);
        const QString key = used == group ? id : used;
        file.groups.insert(key, copy);
        if (layouts.contains(used))
            fileLayouts.insert(key, layouts.value(used));
    }
    if (!fileLayouts.isEmpty()) {
        file.layout.insert("groups", fileLayouts);
        file.present.layout = true;
    }

    // The version: the earlier one bumped as 3.6 says, or the workflow's for a first save.
    const QString folder = dir + QLatin1Char('/') + name;
    SavedGroup saved{id, definition->version, true};
    const Result<QJsonObject> oldManifest = readJsonObject(folder + QStringLiteral("/node.json"));
    // Only the group that came from (or went to) that library entry may update it: another
    // group that happens to have the name would overwrite it.
    if (oldManifest.ok() && definition->extra.value(QLatin1String(kLibraryKey)).toString() != id)
        return libraryError(QStringLiteral("The library has another group named %1.").arg(id),
                            QStringLiteral("Rename this group, then save it to the library."));
    file.groups[id].extra.insert(QLatin1String(kLibraryKey), id);
    const Result<Document> oldFile = Document::loadFile(folder + QLatin1Char('/') + QLatin1String(kDefinition));
    if (oldManifest.ok()) {
        const Result<NodeType> old = NodeType::fromJson(oldManifest.value());
        if (old.ok()) {
            Document same = file;
            same.groups[id].version = old->version;
            if (oldFile.ok() && oldFile->toJson() == same.toJson()) {
                saved.version = old->version;
                saved.changed = false;
                return saved;
            }
            const NodeType now = groupFace(file, registry, id);
            bool broke = false, grew = false;
            compare(ports(old->inputs), ports(now.inputs), &broke, &grew);
            compare(ports(old->outputs), ports(now.outputs), &broke, &grew);
            Version next = old->version;
            if (broke)
                next = Version{next.major + 1, 0, 0};
            else if (grew)
                next = Version{next.major, next.minor + 1, 0};
            else
                next = Version{next.major, next.minor, next.patch + 1};
            // Never below what the workflow has (a definition from a newer library elsewhere).
            saved.version = std::max(next, definition->version);
        }
    }
    file.groups[id].version = saved.version;

    // node.json: the face as a manifest that loads like any other.
    NodeType face = groupFace(file, registry, id);
    face.runtime = QJsonObject{{"kind", "group"}, {"definition", QLatin1String(kDefinition)}};
    const QJsonObject manifest = face.toJson();
    const Result<NodeType> check = NodeType::fromJson(manifest);
    if (!check.ok())
        return libraryError(QStringLiteral("The group's face cannot be a node type: %1").arg(check.error().message));

    if (Errors made = makeFolder(folder); hasErrors(made))
        return made;
    if (Errors written = file.saveFile(folder + QLatin1Char('/') + QLatin1String(kDefinition)); hasErrors(written))
        return written;
    if (Errors written = writeJsonFile(folder + QStringLiteral("/node.json"), manifest); hasErrors(written))
        return written;

    // pack.json lists every folder.
    if (Errors written = writeLibraryPack(dir, libraryFolders(dir) << name); hasErrors(written))
        return written;
    return saved;
}

QList<LibraryGroup> libraryGroups(const QString &dir) {
    QList<LibraryGroup> groups;
    for (const QString &name : libraryFolders(dir)) {
        const Result<QJsonObject> manifest = readJsonObject(dir + QLatin1Char('/') + name + QStringLiteral("/node.json"));
        if (!manifest)
            continue;
        LibraryGroup group;
        group.id = QStringLiteral("local/") + name;
        group.name = manifest->value("name").toString(name);
        group.version = Version::parse(manifest->value("version").toString()).value_or(Version{});
        groups << group;
    }
    return groups;
}

Errors removeFromLibrary(const QString &id, const QString &dir) {
    const QString folder = libraryFolder(id, dir);
    if (folder.isEmpty())
        return {libraryError(QStringLiteral("The library has no group %1.").arg(id))};
    if (!QDir(folder).removeRecursively())
        return {libraryError(QStringLiteral("%1 could not be removed from the library.").arg(id))};
    QStringList folders = libraryFolders(dir);
    folders.removeAll(id.section(QLatin1Char('/'), 1));
    return writeLibraryPack(dir, folders);
}

Result<QString> renameInLibrary(const QString &id, const QString &name, const Registry &registry, const QString &dir) {
    const QString folder = libraryFolder(id, dir);
    if (folder.isEmpty())
        return libraryError(QStringLiteral("The library has no group %1.").arg(id));
    const Result<Document> file = Document::loadFile(folder + QLatin1Char('/') + QLatin1String(kDefinition));
    if (!file)
        return file.errors();
    QString renamedId;
    Result<Document> renamed = renameGroup(*file, id, name, registry, &renamedId);
    if (!renamed)
        return renamed.errors();
    // Its folder follows the id; a name another library group has is refused.
    if (renamedId != id && libraryFolders(dir).contains(renamedId.section(QLatin1Char('/'), 1)))
        return libraryError(QStringLiteral("The library has a group named %1 already.").arg(name.trimmed()),
                            QStringLiteral("Choose another name, or remove that group first."));
    // Saved as the entry it is (so the same id may be written over), then the old folder goes.
    renamed->groups[renamedId].extra.insert(QLatin1String(kLibraryKey), renamedId);
    const Result<SavedGroup> saved = saveToLibrary(*renamed, renamedId, registry, dir);
    if (!saved)
        return saved.errors();
    if (renamedId != id)
        if (Errors removed = removeFromLibrary(id, dir); hasErrors(removed))
            return removed;
    return renamedId;
}

Result<Document> loadLibraryGroup(const NodeType &type) {
    const QString file = type.runtime.value("definition").toString();
    if (type.dir.isEmpty() || file.isEmpty())
        return libraryError(QStringLiteral("%1 is not a group of the library.").arg(type.id));
    Result<Document> loaded = Document::loadFile(QDir(type.dir).filePath(file));
    if (!loaded.ok())
        return loaded;
    if (!loaded->group(type.id)) {
        Error error;
        error.code = QStringLiteral("invalid_document");
        error.message = QStringLiteral("%1 does not hold the group %2.").arg(file, type.id);
        return error;
    }
    return loaded;
}

GroupChanges groupChanges(const Document &workflow, const Document &library, const QString &group,
                          const Registry &registry) {
    GroupChanges changes;
    if (const GroupDefinition *mine = workflow.group(group))
        changes.from = mine->version;
    if (const GroupDefinition *theirs = library.group(group))
        changes.to = theirs->version;
    const NodeType before = groupFace(workflow, registry, group);
    const NodeType after = groupFace(library, registry, group);
    auto diff = [](const QHash<QString, QString> &a, const QHash<QString, QString> &b, QStringList *added,
                   QStringList *removed) {
        for (auto it = a.cbegin(); it != a.cend(); ++it)
            if (!b.contains(it.key()) || b.value(it.key()) != it.value())
                *removed << it.key();
        for (auto it = b.cbegin(); it != b.cend(); ++it)
            if (!a.contains(it.key()) || a.value(it.key()) != it.value())
                *added << it.key();
        added->sort();
        removed->sort();
    };
    diff(ports(before.inputs), ports(after.inputs), &changes.addedInputs, &changes.removedInputs);
    diff(ports(before.outputs), ports(after.outputs), &changes.addedOutputs, &changes.removedOutputs);
    return changes;
}

Document updateGroup(const Document &workflow, const Document &library, const QString &group, const Registry &registry) {
    Document doc = workflow;
    const NodeType before = groupFace(workflow, registry, group);
    const QStringList incoming = usedGroups(library, group);
    // A nested group the workflow holds otherwise under the same id comes in under a new id.
    QHash<QString, QString> renamed;
    for (const QString &id : incoming) {
        if (id == group || !doc.groups.contains(id) || definitionJson(*doc.group(id)) == definitionJson(*library.group(id)))
            continue;
        QString fresh;
        for (int n = 2; fresh.isEmpty() || doc.groups.contains(fresh) || library.groups.contains(fresh); ++n)
            fresh = QStringLiteral("%1-%2").arg(id).arg(n);
        renamed.insert(id, fresh);
    }
    QJsonObject layouts = doc.layout.value("groups").toObject();
    const QJsonObject theirLayouts = library.layout.value("groups").toObject();
    for (const QString &id : incoming) {
        const QString key = renamed.value(id, id);
        if (id != group && doc.groups.contains(key))
            continue; // the same definition
        GroupDefinition definition = *library.group(id);
        for (Node &node : definition.nodes)
            node.type = renamed.value(node.type, node.type);
        doc.groups.insert(key, definition);
        if (theirLayouts.contains(id))
            layouts.insert(key, theirLayouts.value(id));
        else if (id == group)
            layouts.remove(key);
    }
    if (layouts.isEmpty())
        doc.layout.remove("groups");
    else
        doc.layout.insert("groups", layouts);
    doc.present.groups = true;

    // Every instance moves to the new version and loses what the new face lacks or retypes (3.6:
    // what the update listed as going), outwards through the groups around it.
    const GroupDefinition &now = *doc.group(group);
    const NodeType after = groupFace(doc, registry, group);
    const QHash<QString, QString> oldInputs = ports(before.inputs), newInputs = ports(after.inputs);
    const QHash<QString, QString> oldOutputs = ports(before.outputs), newOutputs = ports(after.outputs);
    QSet<QString> goneInputs, goneOutputs;
    for (const GroupInput &input : workflow.group(group) ? workflow.group(group)->inputs : QList<GroupInput>())
        if (!std::any_of(now.inputs.begin(), now.inputs.end(), [&](const GroupInput &i) { return i.name == input.name; })
            || (oldInputs.contains(input.name) && newInputs.value(input.name) != oldInputs.value(input.name)))
            goneInputs.insert(input.name);
    for (const GroupOutput &output : workflow.group(group) ? workflow.group(group)->outputs : QList<GroupOutput>())
        if (!std::any_of(now.outputs.begin(), now.outputs.end(), [&](const GroupOutput &o) { return o.name == output.name; })
            || (oldOutputs.contains(output.name) && newOutputs.value(output.name) != oldOutputs.value(output.name)))
            goneOutputs.insert(output.name);
    const Version version = now.version;
    forEachNodes(doc, [&](Nodes &nodes) {
        for (Node &node : nodes)
            if (node.type == group)
                node.version = version;
    });
    dropExposedPorts(doc, group, goneInputs, goneOutputs);
    pruneGroup(doc, group, registry);
    return doc;
}

QStringList embedGroupTypes(Document &document, const Registry &registry) {
    QStringList embedded;
    for (bool more = true; more;) {
        more = false;
        QStringList wanted;
        forEachNodes(document, [&](Nodes &nodes) {
            for (const Node &node : nodes)
                if (!document.group(node.type) && !wanted.contains(node.type))
                    if (const NodeType *type = registry.nodeType(node.type);
                        type && type->runtime.value("kind") == QLatin1String("group"))
                        wanted << node.type;
        });
        for (const QString &id : wanted) {
            const Result<Document> file = loadLibraryGroup(*registry.nodeType(id));
            if (!file)
                continue;
            // An instance keeps the version it was written with: a different major is the
            // compiler's node_version_mismatch, as for any node type (3.6). Keyed by where it
            // sits: the graph ("") or a definition, then its id.
            QHash<QString, Version> versions;
            for (auto it = document.graph.nodes.cbegin(); it != document.graph.nodes.cend(); ++it)
                if (it->type == id)
                    versions.insert(QLatin1Char('/') + it.key(), it->version);
            for (auto group = document.groups.cbegin(); group != document.groups.cend(); ++group)
                for (auto it = group->nodes.cbegin(); it != group->nodes.cend(); ++it)
                    if (it->type == id)
                        versions.insert(group.key() + QLatin1Char('/') + it.key(), it->version);
            Document holder = document;
            holder.groups.insert(id, GroupDefinition{}); // nothing of the workflow's to keep
            document = updateGroup(holder, *file, id, registry);
            auto restore = [&](const QString &where, Nodes &nodes) {
                for (auto it = nodes.begin(); it != nodes.end(); ++it)
                    if (const QString key = where + QLatin1Char('/') + it.key(); it->type == id && versions.contains(key))
                        it->version = versions.value(key);
            };
            restore({}, document.graph.nodes);
            for (auto group = document.groups.begin(); group != document.groups.end(); ++group)
                restore(group.key(), group->nodes);
            embedded << id;
            more = true;
        }
    }
    return embedded;
}

} // namespace fugue
