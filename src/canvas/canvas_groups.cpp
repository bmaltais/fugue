// The canvas model's group edits (spec 5.2-5.5): Make group, Ungroup, Make unique, and the
// interface of the group shown (expose, rename, reorder). Each is one undo step. The document
// changes themselves are engine/groups.h; this file picks the level they apply to, keeps the
// entered group's face pointing at its nodes, and reports refusals in groupProblem.

#include "canvas/canvas_model.h"

#include "engine/group_library.h"
#include "engine/groups.h"
#include "engine/registry.h"

#include <QJsonArray>
#include <QJsonObject>

using namespace fugue;

namespace {

const Registry &registryOr(const Registry *registry) {
    static const Registry none;
    return registry ? *registry : none;
}

// The exposed input of `group` that drives `target`, or none.
const GroupInput *exposedFor(const GroupDefinition &group, const PortRef &target) {
    for (const GroupInput &input : group.inputs)
        if (input.targets.contains(target))
            return &input;
    return nullptr;
}

// An engine refusal as the window says it: the sentence and how to fix it.
QString problemText(const Error &error) {
    return error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint;
}

QVariant optionalNumber(const std::optional<double> &value) { return value ? QVariant(*value) : QVariant(); }

std::optional<double> numberOf(const QVariant &value) {
    bool ok = false;
    const double number = value.toDouble(&ok);
    return value.isValid() && !value.isNull() && value.toString() != QString() && ok ? std::optional<double>(number)
                                                                                     : std::nullopt;
}

} // namespace

QStringList CanvasModel::selectedSinks() const {
    return sinksAmong(m_document, registryOr(m_registry), selectedIds());
}

void CanvasModel::requestMakeGroup() {
    const QStringList sinks = selectedSinks();
    if (sinks.isEmpty() || sinks.size() == selectedIds().size()) {
        makeGroup(false);
        return;
    }
    QStringList titles;
    for (const QString &sink : sinks)
        titles << nodeTitle(sink);
    emit makeGroupAsked(titles);
}

QString CanvasModel::makeGroup(bool leaveSinks) {
    QStringList ids = selectedIds();
    if (leaveSinks)
        for (const QString &sink : selectedSinks())
            ids.removeAll(sink);
    if (ids.isEmpty()) {
        setGroupProblem(tr("Select the nodes to put in a group first. A sink stays outside the group."));
        return {};
    }
    const Registry &registry = registryOr(m_registry);
    const Result<GroupMade> made = fugue::makeGroup(scopeDocument(), ids, registry);
    if (!made) {
        setGroupProblem(problemText(made.error()));
        return {};
    }
    Document level = made->document;
    // Inside a group: what its face drove or read among the nodes now goes through the new
    // group's face, so the outer instances keep working.
    if (const QString outer = scopeGroup(); !outer.isEmpty() && level.groups.contains(outer)) {
        const QSet<QString> moved(ids.begin(), ids.end());
        GroupDefinition face = level.groups.value(outer);
        for (GroupInput &input : face.inputs) {
            for (PortRef &target : input.targets) {
                if (!moved.contains(target.node))
                    continue;
                QString name;
                if (const GroupInput *inner = exposedFor(*level.group(made->group), target)) {
                    name = inner->name;
                } else {
                    const Result<Document> exposed = fugue::exposeInput(level, made->group, target, registry, {}, &name);
                    if (!exposed)
                        continue;
                    level = *exposed;
                }
                target = PortRef{made->instance, name};
            }
        }
        for (GroupOutput &output : face.outputs) {
            if (!moved.contains(output.from.node))
                continue;
            QString name;
            for (const GroupOutput &inner : level.group(made->group)->outputs)
                if (inner.from == output.from)
                    name = inner.name;
            if (name.isEmpty()) {
                const Result<Document> exposed = fugue::exposeOutput(level, made->group, output.from, &name);
                if (!exposed)
                    continue;
                level = *exposed;
            }
            output.from = PortRef{made->instance, name};
        }
        level.groups[outer].inputs = face.inputs;
        level.groups[outer].outputs = face.outputs;
    }
    setGroupProblem({});
    commit(level, tr("Make group"));
    select({made->instance}, QStringLiteral("replace"));
    return made->instance;
}

QStringList CanvasModel::ungroupSelection() {
    QStringList instances;
    for (const QString &id : selectedIds())
        if (isGroup(id))
            instances << id;
    if (instances.isEmpty())
        return {};
    Document level = scopeDocument();
    const QString outer = scopeGroup();
    QStringList added, groups;
    for (const QString &id : instances) {
        const QString group = level.graph.nodes.value(id).type;
        const GroupDefinition definition = *level.group(group);
        const Result<Ungrouped> back = fugue::ungroup(level, id);
        if (!back) {
            setGroupProblem(problemText(back.error()));
            return {};
        }
        level = back->document;
        added << back->nodes;
        groups << group;
        // Inside a group: what its face drove or read through the instance goes to the inner
        // nodes now in its place.
        if (!outer.isEmpty() && level.groups.contains(outer)) {
            GroupDefinition &face = level.groups[outer];
            for (GroupInput &input : face.inputs) {
                QList<PortRef> targets;
                for (const PortRef &target : input.targets) {
                    if (target.node != id) {
                        targets << target;
                        continue;
                    }
                    for (const GroupInput &inner : definition.inputs)
                        if (inner.name == target.port)
                            for (const PortRef &innerTarget : inner.targets)
                                targets << PortRef{back->ids.value(innerTarget.node), innerTarget.port};
                }
                input.targets = targets;
            }
            for (GroupOutput &output : face.outputs)
                if (output.from.node == id)
                    for (const GroupOutput &inner : definition.outputs)
                        if (inner.name == output.from.port)
                            output.from = PortRef{back->ids.value(inner.from.node), inner.from.port};
        }
    }
    setGroupProblem({});
    beginEdit();
    commit(level, tr("Ungroup"));
    // The definitions nothing uses any more go with them.
    Document whole = document();
    dropUnusedGroups(whole, groups);
    if (whole.groups.size() != document().groups.size())
        commitWhole(whole, tr("Ungroup"));
    select(added, QStringLiteral("replace"));
    endEdit(tr("Ungroup"));
    return added;
}

QString CanvasModel::makeUnique() {
    const QStringList ids = selectedIds();
    if (ids.size() != 1 || !isGroup(ids.first()))
        return {};
    const Result<GroupMade> made = fugue::makeUnique(scopeDocument(), ids.first(), registryOr(m_registry));
    if (!made) {
        setGroupProblem(problemText(made.error()));
        return {};
    }
    setGroupProblem({});
    commit(made->document, tr("Make unique"));
    return made->group;
}

// --- the interface of the group shown (5.4) -----------------------------------------

// A change of the group shown's face (on the whole workflow, so instances elsewhere follow):
// committed as one step named `text`, or refused with its reason.
bool CanvasModel::applyFaceEdit(const std::function<Result<Document>(const Document &, const QString &)> &edit,
                                const QString &text) {
    const QString group = scopeGroup();
    if (group.isEmpty())
        return false;
    const Result<Document> done = edit(document(), group);
    if (!done) {
        setGroupProblem(problemText(done.error()));
        return false;
    }
    setGroupProblem({});
    commitWhole(*done, text);
    return true;
}

bool CanvasModel::exposeInput(const QString &node, const QString &input, const QString &into) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::exposeInput(whole, group, {node, input}, registryOr(m_registry), into);
        },
        tr("Expose input"));
}

bool CanvasModel::unexposeInput(const QString &node, const QString &input) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::unexposeInput(whole, group, {node, input});
        },
        tr("Stop exposing"));
}

bool CanvasModel::exposeOutput(const QString &node, const QString &output) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::exposeOutput(whole, group, {node, output});
        },
        tr("Expose output"));
}

bool CanvasModel::unexposeOutput(const QString &name) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::unexposeOutput(whole, group, name);
        },
        tr("Stop exposing"));
}

QVariantMap CanvasModel::portToRename(const QString &node, const QString &port, bool output) const {
    QString group, name;
    if (isGroup(node)) { // a port of a group card, at this level
        group = m_document.graph.nodes.value(node).type;
        name = port;
    } else if (inGroup()) { // an inner port the group shown exposes
        group = scopeGroup();
        name = exposedName(node, port, output);
    }
    if (group.isEmpty() || name.isEmpty())
        return {};
    const NodeType face = groupFace(document(), registryOr(m_registry), group);
    QString label;
    if (output) {
        const OutputSpec *spec = face.output(name);
        if (!spec)
            return {};
        label = spec->label.isEmpty() ? spec->name : spec->label;
    } else {
        const InputSpec *spec = face.input(name);
        if (!spec)
            return {};
        label = spec->label.isEmpty() ? spec->name : spec->label;
    }
    return {{"group", group}, {"name", name}, {"output", output}, {"label", label}};
}

bool CanvasModel::renamePort(const QString &node, const QString &port, bool output, const QString &label) {
    const QVariantMap target = portToRename(node, port, output);
    if (target.isEmpty())
        return false;
    const Result<Document> done = relabelExposed(document(), target.value("group").toString(), output,
                                                 target.value("name").toString(), label);
    if (!done) {
        setGroupProblem(problemText(done.error()));
        return false;
    }
    setGroupProblem({});
    commitWhole(*done, output ? tr("Rename output") : tr("Rename input"));
    return true;
}

QString CanvasModel::exposedName(const QString &node, const QString &port, bool output) const {
    const GroupDefinition *group = m_document.group(scopeGroup());
    if (!group)
        return {};
    if (output) {
        for (const GroupOutput &exposed : group->outputs)
            if (exposed.from == PortRef{node, port})
                return exposed.name;
        return {};
    }
    const GroupInput *exposed = exposedFor(*group, {node, port});
    return exposed ? exposed->name : QString();
}

bool CanvasModel::removeExposedInput(const QString &name) {
    const QString group = scopeGroup();
    Document whole = document();
    const GroupDefinition *definition = whole.group(group);
    if (!definition)
        return false;
    for (const GroupInput &exposed : definition->inputs) {
        if (exposed.name != name)
            continue;
        for (const PortRef &target : exposed.targets) {
            const Result<Document> done = fugue::unexposeInput(whole, group, target);
            if (done)
                whole = *done;
        }
        setGroupProblem({});
        commitWhole(whole, tr("Stop exposing"));
        return true;
    }
    return false;
}

QVariantList CanvasModel::exposeCandidates(const QString &name) const {
    const GroupDefinition *group = m_document.group(scopeGroup());
    if (!group)
        return {};
    QString type;
    for (const GroupInput &exposed : group->inputs)
        if (exposed.name == name && !exposed.targets.isEmpty())
            if (const std::optional<PortSpec> spec = portType(exposed.targets.first().node, exposed.targets.first().port, false))
                type = spec->toString();
    if (type.isEmpty())
        return {};
    QVariantList candidates;
    for (const NodeRow &row : m_rows)
        for (const Port &port : row.inputs)
            if (port.type && port.type->toString() == type && !exposedFor(*group, {row.id, port.name}))
                candidates.append(QVariantMap{{"node", row.id}, {"input", port.name},
                                              {"title", row.title + QStringLiteral(" \u203a ") + port.label}});
    return candidates;
}

QVariantMap CanvasModel::groupInterface() const {
    const QString id = scopeGroup();
    const Document whole = document();
    const GroupDefinition *group = whole.group(id);
    if (!group)
        return {};
    const NodeType face = groupFace(whole, registryOr(m_registry), id);
    QVariantList inputs, outputs;
    for (const GroupInput &input : group->inputs) {
        QStringList targets;
        for (const PortRef &target : input.targets)
            targets << target.toString();
        const InputSpec *spec = face.input(input.name);
        inputs.append(QVariantMap{{"name", input.name},
                                  {"label", input.label},
                                  {"section", input.section},
                                  {"min", optionalNumber(input.min)},
                                  {"max", optionalNumber(input.max)},
                                  {"default", input.defaultValue ? input.defaultValue->literal().toVariant() : QVariant()},
                                  {"choices", input.choices ? input.choices->join(QStringLiteral(", ")) : QString()},
                                  {"innerChoices", spec && spec->choices && !input.choices ? spec->choices->join(QStringLiteral(", "))
                                                   : QString()},
                                  {"targets", targets},
                                  {"type", spec ? spec->type.toString() : QString()}});
    }
    for (const GroupOutput &output : group->outputs) {
        const OutputSpec *spec = face.output(output.name);
        outputs.append(QVariantMap{{"name", output.name},
                                   {"from", output.from.toString()},
                                   {"type", spec ? spec->type.toString() : QString()}});
    }
    return {{"id", id},
            {"name", group->name},
            {"version", group->version.toString()},
            {"inputs", inputs},
            {"outputs", outputs}};
}

bool CanvasModel::editExposedInput(const QString &name, const QVariantMap &change) {
    ExposedChange edit;
    edit.name = change.value("name").toString().trimmed();
    edit.label = change.value("label").toString().trimmed();
    edit.section = change.value("section").toString().trimmed();
    edit.min = numberOf(change.value("min"));
    edit.max = numberOf(change.value("max"));
    // The default as text from the editor: a number for a number input, else the text; empty: none.
    const QString type = change.value("type").toString();
    const QString given = change.value("default").toString().trimmed();
    if (change.value("default").isValid() && !given.isEmpty()) {
        bool isNumber = false;
        const double number = given.toDouble(&isNumber);
        edit.defaultValue = type == QLatin1String("number") && isNumber ? QJsonValue(number)
                            : type == QLatin1String("boolean")         ? QJsonValue(given == QLatin1String("true"))
                                                                       : QJsonValue(given);
    }
    if (const QString choices = change.value("choices").toString().trimmed(); !choices.isEmpty()) {
        QStringList list;
        for (const QString &choice : choices.split(QLatin1Char(',')))
            if (!choice.trimmed().isEmpty())
                list << choice.trimmed();
        edit.choices = list;
    }
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::editExposedInput(whole, group, name, edit, registryOr(m_registry));
        },
        tr("Edit exposed input"));
}

bool CanvasModel::renameExposedOutput(const QString &name, const QString &newName) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::renameExposedOutput(whole, group, name, newName.trimmed());
        },
        tr("Rename output"));
}

bool CanvasModel::moveExposed(bool output, const QString &name, int to) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::moveExposed(whole, group, output, name, to);
        },
        output ? tr("Move output") : tr("Move input"));
}

bool CanvasModel::renameGroup(const QString &name) {
    const bool done = applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::renameGroup(whole, group, name, registryOr(m_registry));
        },
        tr("Rename group"));
    if (done)
        emit scopeChanged(); // the breadcrumb shows the name
    return done;
}

// --- sections (5.4, 6.1) ---------------------------------------------------------------

QString CanvasModel::advancedHeaderAt(qreal x, qreal y) const {
    for (int r = int(m_rows.size()) - 1; r >= 0; --r) {
        const NodeRow &row = m_rows[size_t(r)];
        if (row.collapsed || x < row.pos.x() || x > row.pos.x() + row.shownWidth())
            continue;
        for (const Port &port : row.inputs)
            if (port.header == QLatin1String(kAdvancedSection) && y >= row.pos.y() + port.headerTop
                && y < row.pos.y() + port.headerTop + m_rowHeight)
                return row.id;
    }
    return {};
}

void CanvasModel::toggleAdvanced(const QString &node) {
    const int r = rowOf(node);
    if (r < 0)
        return;
    Document document = scopeDocument();
    QJsonObject entries = document.layout.value("nodes").toObject();
    QJsonObject entry = entries.value(node).toObject();
    const bool open = !m_rows[size_t(r)].advancedOpen;
    if (open)
        entry.insert("advanced", true);
    else
        entry.remove("advanced");
    entries.insert(node, entry);
    document.layout.insert("nodes", entries);
    commit(document, open ? tr("Show advanced inputs") : tr("Fold advanced inputs"));
}

// --- the library (5.3) ---------------------------------------------------------------

void CanvasModel::setGroupNotice(const QString &notice) {
    if (notice == m_groupNotice)
        return;
    m_groupNotice = notice;
    emit groupNoticeChanged();
}

bool CanvasModel::saveGroupToLibrary() {
    QString group;
    const QStringList ids = selectedIds();
    if (ids.size() == 1 && isGroup(ids.first()))
        group = m_document.graph.nodes.value(ids.first()).type;
    else
        group = scopeGroup();
    if (group.isEmpty())
        return false;
    const Registry &registry = registryOr(m_registry);
    Document whole = document();
    // Saved as local/<name>: a different group of this workflow with that id would be overwritten.
    const QString target = QStringLiteral("local/") + group.section(QLatin1Char('/'), 1);
    if (target != group && whole.groups.contains(target)) {
        setGroupProblem(tr("This workflow has another group with the id %1; rename one of them first.").arg(target));
        return false;
    }
    const Result<SavedGroup> saved =
        saveToLibrary(whole, group, registry, m_libraryDir.isEmpty() ? libraryDir() : m_libraryDir);
    if (!saved) {
        setGroupProblem(problemText(saved.error()));
        return false;
    }
    setGroupProblem({});
    // The workflow takes the library's id and version, so it is in step with it.
    whole = renameGroupId(whole, group, saved->id);
    whole.groups[saved->id].version = saved->version;
    whole.groups[saved->id].extra.insert(QLatin1String(kLibraryKey), saved->id);
    forEachNodes(whole, [&](Nodes &nodes) {
        for (Node &node : nodes)
            if (node.type == saved->id)
                node.version = saved->version;
    });
    commitWhole(whole, tr("Save to library"));
    emit scopeChanged(); // the breadcrumb's group may have a new id
    setGroupNotice(saved->changed ? tr("Saved %1 %2 to the library. Every open workflow can add it from node search.")
                                        .arg(saved->id, saved->version.toString())
                                  : tr("The library has this version of %1 already (%2).")
                                        .arg(saved->id, saved->version.toString()));
    if (saved->changed)
        emit librarySaved();
    return true;
}

QString CanvasModel::updateAvailable(const QString &node) const {
    const auto it = m_document.graph.nodes.constFind(node);
    if (it == m_document.graph.nodes.constEnd() || !m_registry)
        return {};
    const GroupDefinition *group = m_document.group(it->type);
    const NodeType *library = m_registry->nodeType(it->type);
    if (!group || !library || library->runtime.value("kind") != QLatin1String("group") || library->version <= group->version)
        return {};
    return library->version.toString();
}

QVariantMap CanvasModel::groupUpdate(const QString &node) const {
    if (updateAvailable(node).isEmpty())
        return {};
    const QString group = m_document.graph.nodes.value(node).type;
    const Result<Document> library = loadLibraryGroup(*m_registry->nodeType(group));
    if (!library)
        return {{"problem", library.error().message}};
    const GroupChanges changes = groupChanges(document(), *library, group, *m_registry);
    return {{"name", m_document.group(group)->name},
            {"from", changes.from.toString()},
            {"to", changes.to.toString()},
            {"addedInputs", changes.addedInputs},
            {"removedInputs", changes.removedInputs},
            {"addedOutputs", changes.addedOutputs},
            {"removedOutputs", changes.removedOutputs}};
}

bool CanvasModel::updateGroupInstance(const QString &node) {
    if (updateAvailable(node).isEmpty())
        return false;
    const QString group = m_document.graph.nodes.value(node).type;
    const Result<Document> library = loadLibraryGroup(*m_registry->nodeType(group));
    if (!library) {
        setGroupProblem(problemText(library.error()));
        return false;
    }
    setGroupProblem({});
    commitWhole(updateGroup(document(), *library, group, *m_registry), tr("Update group"));
    return true;
}

void CanvasModel::requestGroupUpdate() {
    const QStringList ids = selectedIds();
    if (ids.size() == 1 && !updateAvailable(ids.first()).isEmpty())
        emit groupUpdateAsked(ids.first(), groupUpdate(ids.first()));
}
