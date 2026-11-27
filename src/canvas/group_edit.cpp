// GroupEdit: the group edits (spec 5.2-5.5) split out of the canvas model: entering and
// leaving a level, Make group, Ungroup, Make unique, the interface of the group shown (expose,
// rename, reorder) and the library. Each edit is one undo step. The document changes
// themselves are engine/groups.h; this file picks the level they apply to, keeps the entered
// group's face pointing at its nodes, and reports refusals in `problem`. It reaches the model
// through the commit path WidgetEdit uses (friend), and keeps no state of the level: the
// scope is the model's.

#include "canvas/group_edit.h"

#include "canvas/canvas_model.h"
#include "engine/group_library.h"
#include "engine/groups.h"
#include "engine/registry.h"
#include "typed_number.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

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

// "A", "A and B", "A, B and C": a list as a sentence says it.
QString spokenList(const QStringList &items) {
    if (items.size() <= 1)
        return items.value(0);
    QString head = items.first();
    for (qsizetype i = 1; i + 1 < items.size(); ++i)
        head = GroupEdit::tr("%1, %2").arg(head, items.at(i));
    return GroupEdit::tr("%1 and %2").arg(head, items.last());
}

} // namespace
GroupEdit::GroupEdit(const Registry *registry, CanvasModel *canvas, QObject *parent)
    : QObject(parent), m_registry(registry), m_canvas(canvas) {}

QString GroupEdit::libraryPath() const { return m_libraryDir.isEmpty() ? libraryDir() : m_libraryDir; }

void GroupEdit::dismiss() {
    setProblem({});
    setNotice({});
}

void GroupEdit::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit problemChanged();
}

void GroupEdit::setNotice(const QString &notice) {
    if (notice == m_notice)
        return;
    m_notice = notice;
    emit noticeChanged();
}

void GroupEdit::announceLibraryResult(const QString &notice, const QString &problem) {
    setProblem(problem);
    setNotice(problem.isEmpty() ? notice : QString());
}

// --- the level shown (5.2) -------------------------------------------------------------

bool GroupEdit::enterGroup(const QString &instance) {
    if (!m_canvas->isGroup(instance))
        return false;
    m_canvas->finishGestures();
    const Document whole = m_canvas->document();
    m_canvas->m_outerViews.push_back(m_canvas->m_view);
    m_canvas->m_root = whole;
    m_canvas->m_scope << instance;
    const QVariantMap none;
    m_canvas->loadLevel(*CanvasModel::levelOf(m_canvas->m_root, m_canvas->m_scope), &none);
    emit m_canvas->scopeChanged();
    m_canvas->requestFit();
    return true;
}

bool GroupEdit::leaveGroup() {
    if (m_canvas->m_scope.isEmpty())
        return false;
    m_canvas->finishGestures();
    const Document whole = m_canvas->document();
    const QString left = m_canvas->m_scope.takeLast();
    QVariantMap view;
    if (!m_canvas->m_outerViews.empty()) {
        view = m_canvas->m_outerViews.back();
        m_canvas->m_outerViews.pop_back();
    }
    if (m_canvas->m_scope.isEmpty()) {
        m_canvas->m_root = Document();
        m_canvas->loadLevel(whole, &view);
    } else {
        m_canvas->m_root = whole;
        m_canvas->loadLevel(*CanvasModel::levelOf(m_canvas->m_root, m_canvas->m_scope), &view);
    }
    m_canvas->select({left}, QStringLiteral("replace"));
    emit m_canvas->scopeChanged();
    return true;
}

void GroupEdit::leaveTo(int depth) {
    while (m_canvas->m_scope.size() > std::max(0, depth))
        leaveGroup();
}

QStringList GroupEdit::selectedSinks() const {
    return sinksAmong(m_canvas->m_document, registryOr(m_registry), m_canvas->selectedIds());
}

void GroupEdit::requestMakeGroup() {
    const QStringList sinks = selectedSinks();
    if (sinks.isEmpty() || sinks.size() == m_canvas->selectedIds().size()) {
        makeGroup(false);
        return;
    }
    QStringList titles;
    for (const QString &sink : sinks)
        titles << m_canvas->nodeTitle(sink);
    emit makeGroupAsked(titles);
}

QString GroupEdit::makeGroup(bool leaveSinks) {
    QStringList ids = m_canvas->selectedIds();
    if (leaveSinks)
        for (const QString &sink : selectedSinks())
            ids.removeAll(sink);
    if (ids.isEmpty()) {
        setProblem(tr("Select the nodes to put in a group first. A sink stays outside the group."));
        return {};
    }
    const Registry &registry = registryOr(m_registry);
    const Result<GroupMade> made = fugue::makeGroup(m_canvas->scopeDocument(), ids, registry);
    if (!made) {
        setProblem(problemText(made.error()));
        return {};
    }
    Document level = made->document;
    // Inside a group: what its face drove or read among the nodes now goes through the new
    // group's face, so the outer instances keep working.
    if (const QString outer = m_canvas->scopeGroup(); !outer.isEmpty() && level.groups.contains(outer)) {
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
    setProblem({});
    m_canvas->commit(level, tr("Make group"));
    m_canvas->select({made->instance}, QStringLiteral("replace"));
    return made->instance;
}

QStringList GroupEdit::ungroupSelection() {
    QStringList instances;
    for (const QString &id : m_canvas->selectedIds())
        if (m_canvas->isGroup(id))
            instances << id;
    if (instances.isEmpty())
        return {};
    Document level = m_canvas->scopeDocument();
    const QString outer = m_canvas->scopeGroup();
    QStringList added, groups;
    for (const QString &id : instances) {
        const QString group = level.graph.nodes.value(id).type;
        const GroupDefinition definition = *level.group(group);
        const Result<Ungrouped> back = fugue::ungroup(level, id);
        if (!back) {
            setProblem(problemText(back.error()));
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
    setProblem({});
    m_canvas->beginEdit();
    m_canvas->commit(level, tr("Ungroup"));
    // The definitions nothing uses any more go with them.
    Document whole = m_canvas->document();
    dropUnusedGroups(whole, groups);
    if (whole.groups.size() != m_canvas->document().groups.size())
        m_canvas->commitWhole(whole, tr("Ungroup"));
    m_canvas->select(added, QStringLiteral("replace"));
    m_canvas->endEdit(tr("Ungroup"));
    return added;
}

QString GroupEdit::makeUnique() {
    const QStringList ids = m_canvas->selectedIds();
    if (ids.size() != 1 || !m_canvas->isGroup(ids.first()))
        return {};
    const Result<GroupMade> made = fugue::makeUnique(m_canvas->scopeDocument(), ids.first(), registryOr(m_registry));
    if (!made) {
        setProblem(problemText(made.error()));
        return {};
    }
    setProblem({});
    m_canvas->commit(made->document, tr("Make unique"));
    return made->group;
}

// --- the interface of the group shown (5.4) -----------------------------------------

// A change of the group shown's face (on the whole workflow, so instances elsewhere follow):
// committed as one step named `text`, or refused with its reason.
bool GroupEdit::applyFaceEdit(const std::function<Result<Document>(const Document &, const QString &)> &edit,
                                const QString &text) {
    const QString group = m_canvas->scopeGroup();
    if (group.isEmpty())
        return false;
    const Result<Document> done = edit(m_canvas->document(), group);
    if (!done) {
        setProblem(problemText(done.error()));
        return false;
    }
    setProblem({});
    m_canvas->commitWhole(*done, text);
    return true;
}

bool GroupEdit::exposeInput(const QString &node, const QString &input, const QString &into) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::exposeInput(whole, group, {node, input}, registryOr(m_registry), into);
        },
        tr("Expose input"));
}

bool GroupEdit::unexposeInput(const QString &node, const QString &input) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::unexposeInput(whole, group, {node, input});
        },
        tr("Stop exposing"));
}

bool GroupEdit::exposeOutput(const QString &node, const QString &output) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::exposeOutput(whole, group, {node, output});
        },
        tr("Expose output"));
}

bool GroupEdit::unexposeOutput(const QString &name) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::unexposeOutput(whole, group, name);
        },
        tr("Stop exposing"));
}

bool GroupEdit::relabelExposedOutput(const QString &name, const QString &label) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return relabelExposed(whole, group, true, name, label.trimmed());
        },
        tr("Rename output"));
}

QVariantMap GroupEdit::portToRename(const QString &node, const QString &port, bool output) const {
    QString group, name;
    if (m_canvas->isGroup(node)) { // a port of a group card, at this level
        group = m_canvas->m_document.graph.nodes.value(node).type;
        name = port;
    } else if (m_canvas->inGroup()) { // an inner port the group shown exposes
        group = m_canvas->scopeGroup();
        name = exposedName(node, port, output);
    }
    if (group.isEmpty() || name.isEmpty())
        return {};
    const NodeType face = groupFace(m_canvas->document(), registryOr(m_registry), group);
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

bool GroupEdit::renamePort(const QString &node, const QString &port, bool output, const QString &label) {
    const QVariantMap target = portToRename(node, port, output);
    if (target.isEmpty())
        return false;
    const Result<Document> done = relabelExposed(m_canvas->document(), target.value("group").toString(), output,
                                                 target.value("name").toString(), label);
    if (!done) {
        setProblem(problemText(done.error()));
        return false;
    }
    setProblem({});
    m_canvas->commitWhole(*done, output ? tr("Rename output") : tr("Rename input"));
    return true;
}

QString GroupEdit::exposedName(const QString &node, const QString &port, bool output) const {
    const GroupDefinition *group = m_canvas->m_document.group(m_canvas->scopeGroup());
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

bool GroupEdit::removeExposedInput(const QString &name) {
    const QString group = m_canvas->scopeGroup();
    Document whole = m_canvas->document();
    const GroupDefinition *definition = whole.group(group);
    if (!definition)
        return false;
    for (const GroupInput &exposed : definition->inputs) {
        if (exposed.name != name)
            continue;
        // A copy: each step replaces `whole`, which `definition` points into.
        const QList<PortRef> targets = exposed.targets;
        for (const PortRef &target : targets) {
            const Result<Document> done = fugue::unexposeInput(whole, group, target);
            if (done)
                whole = *done;
        }
        setProblem({});
        m_canvas->commitWhole(whole, tr("Stop exposing"));
        return true;
    }
    setProblem(tr("The group has no exposed input %1.").arg(name));
    return false;
}

QVariantList GroupEdit::exposeCandidates(const QString &name) const {
    const GroupDefinition *group = m_canvas->m_document.group(m_canvas->scopeGroup());
    if (!group)
        return {};
    QString type;
    for (const GroupInput &exposed : group->inputs)
        if (exposed.name == name && !exposed.targets.isEmpty())
            if (const std::optional<PortSpec> spec = m_canvas->portType(exposed.targets.first().node, exposed.targets.first().port, false))
                type = spec->toString();
    if (type.isEmpty())
        return {};
    QVariantList candidates;
    for (const CanvasModel::NodeRow &row : m_canvas->m_rows)
        for (const CanvasModel::Port &port : row.inputs)
            if (port.type && port.type->toString() == type && !exposedFor(*group, {row.id, port.name}))
                candidates.append(QVariantMap{{"node", row.id}, {"input", port.name},
                                              {"title", row.title + QStringLiteral(" \u203a ") + port.label}});
    return candidates;
}

GroupEdit::PortWords GroupEdit::portWords(const PortRef &port, bool output) const {
    for (const CanvasModel::NodeRow &row : m_canvas->m_rows) {
        if (row.id != port.node)
            continue;
        for (const CanvasModel::Port &candidate : output ? row.outputs : row.inputs)
            if (candidate.name == port.port)
                return {candidate.label.isEmpty() ? candidate.name : candidate.label, row.title.isEmpty() ? row.id : row.title};
        return {port.port, row.title.isEmpty() ? row.id : row.title};
    }
    return {port.port, port.node};
}

QString GroupEdit::setsText(const QList<PortRef> &targets) const {
    // The inner inputs by their label, in order: "Model on Encode and Sample", "Seed on Sample and Steps on Refine".
    QStringList labels;
    QMap<QString, QStringList> titles;
    for (const PortRef &target : targets) {
        const PortWords words = portWords(target, false);
        if (!labels.contains(words.port))
            labels << words.port;
        titles[words.port] << words.node;
    }
    QStringList parts;
    for (const QString &label : std::as_const(labels))
        parts << tr("%1 on %2").arg(label, spokenList(titles.value(label)));
    return parts.isEmpty() ? tr("Sets nothing inside") : tr("Sets %1").arg(spokenList(parts));
}

QString GroupEdit::comesFromText(const PortRef &from) const {
    const PortWords words = portWords(from, true);
    return tr("From %1 on %2").arg(words.port, words.node);
}

QVariantList GroupEdit::exposeChoices(bool output) const {
    const GroupDefinition *group = m_canvas->m_document.group(m_canvas->scopeGroup());
    if (!group)
        return {};
    QSet<QString> exposed;
    if (output)
        for (const GroupOutput &port : group->outputs)
            exposed.insert(port.from.toString());
    else
        for (const GroupInput &port : group->inputs)
            for (const PortRef &target : port.targets)
                exposed.insert(target.toString());
    QVariantList choices;
    for (const CanvasModel::NodeRow &row : m_canvas->m_rows)
        for (const CanvasModel::Port &port : output ? row.outputs : row.inputs)
            if (!exposed.contains(PortRef{row.id, port.name}.toString()))
                choices.append(QVariantMap{{"node", row.id}, {"port", port.name},
                                           {"title", (row.title.isEmpty() ? row.id : row.title) + QStringLiteral(" \u203a ")
                                                         + (port.label.isEmpty() ? port.name : port.label)}});
    return choices;
}

QVariantMap GroupEdit::groupInterface() const {
    const QString id = m_canvas->scopeGroup();
    const Document whole = m_canvas->document();
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
                                  {"sets", setsText(input.targets)},
                                  {"shown", spec && !spec->label.isEmpty() ? spec->label : input.name},
                                  {"type", spec ? spec->type.toString() : QString()}});
    }
    for (const GroupOutput &output : group->outputs) {
        const OutputSpec *spec = face.output(output.name);
        outputs.append(QVariantMap{{"name", output.name},
                                   {"label", output.label},
                                   {"from", output.from.toString()},
                                   {"fromNode", output.from.node},
                                   {"fromPort", output.from.port},
                                   {"comesFrom", comesFromText(output.from)},
                                   {"shown", spec && !spec->label.isEmpty() ? spec->label : output.name},
                                   {"type", spec ? spec->type.toString() : QString()}});
    }
    return {{"id", id},
            {"name", group->name},
            {"version", group->version.toString()},
            {"inputs", inputs},
            {"outputs", outputs}};
}

bool GroupEdit::editExposedInput(const QString &name, const QVariantMap &change) {
    ExposedChange edit;
    edit.name = change.value("name").toString().trimmed();
    edit.label = change.value("label").toString().trimmed();
    edit.section = change.value("section").toString().trimmed();
    // A range limit: a number, or empty for none. Text that is not a whole number (a comma the
    // language no longer reads as a decimal point, "1e999") is refused, never kept as no limit.
    QString refused;
    const auto limit = [&](const QVariant &value) -> std::optional<double> {
        if (!value.isValid() || value.isNull())
            return std::nullopt;
        if (value.typeId() == QMetaType::Double || value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong)
            return value.toDouble();
        const QString text = value.toString().trimmed();
        if (text.isEmpty())
            return std::nullopt;
        const std::optional<double> number = TypedNumber::read(text, QLocale::c());
        if (!number && refused.isEmpty())
            refused = text;
        return number;
    };
    edit.min = limit(change.value("min"));
    edit.max = limit(change.value("max"));
    if (!refused.isEmpty()) {
        setProblem(tr("\u201c%1\u201d is not a number: type one, as 0.5, or leave the field empty for no limit.").arg(refused));
        return false;
    }
    // The default as text from the editor: a number for a number input, else the text; empty: none.
    const QString type = change.value("type").toString();
    const QString given = change.value("default").toString().trimmed();
    if (change.value("default").isValid() && !given.isEmpty()) {
        // A number input's default is read by the one rule for typed numbers, in the language's own decimal point ("0,5" in
        // Danish); text that is not a number goes on as text and the type check refuses it.
        const std::optional<double> number = TypedNumber::read(given, QLocale());
        edit.defaultValue = type == QLatin1String("number") && number ? QJsonValue(*number)
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

bool GroupEdit::renameExposedOutput(const QString &name, const QString &newName) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::renameExposedOutput(whole, group, name, newName.trimmed());
        },
        tr("Rename output"));
}

bool GroupEdit::moveExposed(bool output, const QString &name, int to) {
    return applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::moveExposed(whole, group, output, name, to);
        },
        output ? tr("Move output") : tr("Move input"));
}

bool GroupEdit::renameGroup(const QString &name) {
    const bool done = applyFaceEdit(
        [&](const Document &whole, const QString &group) {
            return fugue::renameGroup(whole, group, name, registryOr(m_registry));
        },
        tr("Rename group"));
    if (done) {
        emit m_canvas->scopeChanged(); // the breadcrumb shows the name
        const GroupDefinition *definition = m_canvas->document().group(m_canvas->scopeGroup());
        const QString library = definition ? definition->extra.value(QLatin1String(kLibraryKey)).toString() : QString();
        if (!library.isEmpty())
            emit libraryRenameOffered(library, name.trimmed());
    }
    return done;
}

// --- the library (5.3) ---------------------------------------------------------------

void GroupEdit::renameGroupCard(const QString &node, const QString &name) {
    const QString shown = name.trimmed();
    const QString group = m_canvas->m_document.graph.nodes.value(node).type;
    const GroupDefinition *definition = m_canvas->isGroup(node) ? m_canvas->document().group(group) : nullptr;
    if (shown.isEmpty() || !definition || definition->name == shown)
        return; // nothing to rename (F2 then Enter): no step, no question
    QString renamedId;
    const Result<Document> renamed = fugue::renameGroup(m_canvas->document(), group, shown, registryOr(m_registry), &renamedId);
    if (!renamed) {
        setProblem(problemText(renamed.error()));
        return;
    }
    setProblem({});
    m_canvas->beginEdit();
    m_canvas->commitWhole(*renamed, tr("Rename group"));
    // The card shows the group's name: a title of its own would hide the new one.
    Document level = m_canvas->scopeDocument();
    QJsonObject entries = level.layout.value("nodes").toObject();
    if (QJsonObject entry = entries.value(node).toObject(); entry.contains("title")) {
        entry.remove("title");
        entries.insert(node, entry);
        level.layout.insert("nodes", entries);
        m_canvas->commit(level, tr("Rename group"));
    }
    m_canvas->endEdit(tr("Rename group"));
    emit m_canvas->scopeChanged(); // the breadcrumb shows the name
    if (const GroupDefinition *renamedDefinition = m_canvas->document().group(renamedId))
        if (const QString library = renamedDefinition->extra.value(QLatin1String(kLibraryKey)).toString();
            !library.isEmpty())
            emit libraryRenameOffered(library, shown);
}

bool GroupEdit::renameLibraryGroup(const QString &libraryId, const QString &name) {
    const Result<QString> renamed = renameInLibrary(libraryId, name, registryOr(m_registry),
                                                    libraryPath());
    if (!renamed) {
        setProblem(problemText(renamed.error()));
        return false;
    }
    setProblem({});
    followLibraryRename(libraryId, *renamed);
    setNotice(tr("Renamed %1 to %2 in the library.").arg(libraryId, *renamed));
    emit librarySaved();
    return true;
}

void GroupEdit::followLibraryRename(const QString &from, const QString &to) {
    m_canvas->rewriteDocuments([&](Document &document) {
        for (GroupDefinition &definition : document.groups)
            if (definition.extra.value(QLatin1String(kLibraryKey)).toString() == from)
                definition.extra.insert(QLatin1String(kLibraryKey), to);
    });
}

bool GroupEdit::saveGroupToLibrary() {
    QString group;
    const QStringList ids = m_canvas->selectedIds();
    if (ids.size() == 1 && m_canvas->isGroup(ids.first()))
        group = m_canvas->m_document.graph.nodes.value(ids.first()).type;
    else
        group = m_canvas->scopeGroup();
    if (group.isEmpty())
        return false;
    const Registry &registry = registryOr(m_registry);
    Document whole = m_canvas->document();
    // Saved as local/<name>: a different group of this workflow with that id would be overwritten.
    const QString target = QStringLiteral("local/") + group.section(QLatin1Char('/'), 1);
    if (target != group && whole.groups.contains(target)) {
        setProblem(tr("This workflow has another group with the id %1; rename one of them first.").arg(target));
        return false;
    }
    const Result<SavedGroup> saved =
        saveToLibrary(whole, group, registry, libraryPath());
    if (!saved) {
        setProblem(problemText(saved.error()));
        return false;
    }
    setProblem({});
    // The workflow takes the library's id and version, so it is in step with it.
    whole = renameGroupId(whole, group, saved->id);
    whole.groups[saved->id].version = saved->version;
    whole.groups[saved->id].extra.insert(QLatin1String(kLibraryKey), saved->id);
    forEachNodes(whole, [&](Nodes &nodes) {
        for (Node &node : nodes)
            if (node.type == saved->id)
                node.version = saved->version;
    });
    m_canvas->commitWhole(whole, tr("Save to library"));
    emit m_canvas->scopeChanged(); // the breadcrumb's group may have a new id
    setNotice(saved->changed ? tr("Saved %1 %2 to the library. Every open workflow can add it from node search.")
                                        .arg(saved->id, saved->version.toString())
                                  : tr("The library has this version of %1 already (%2).")
                                        .arg(saved->id, saved->version.toString()));
    if (saved->changed)
        emit librarySaved();
    return true;
}

QVariantMap GroupEdit::groupUpdate(const QString &node) const {
    if (m_canvas->updateAvailable(node).isEmpty())
        return {};
    const QString group = m_canvas->m_document.graph.nodes.value(node).type;
    const Result<Document> library = loadLibraryGroup(*m_registry->nodeType(group));
    if (!library)
        return {{"problem", library.error().message}};
    const GroupChanges changes = groupChanges(m_canvas->document(), *library, group, *m_registry);
    return {{"name", m_canvas->m_document.group(group)->name},
            {"from", changes.from.toString()},
            {"to", changes.to.toString()},
            {"addedInputs", changes.addedInputs},
            {"removedInputs", changes.removedInputs},
            {"addedOutputs", changes.addedOutputs},
            {"removedOutputs", changes.removedOutputs}};
}

bool GroupEdit::updateGroupInstance(const QString &node) {
    if (m_canvas->updateAvailable(node).isEmpty())
        return false;
    const QString group = m_canvas->m_document.graph.nodes.value(node).type;
    const Result<Document> library = loadLibraryGroup(*m_registry->nodeType(group));
    if (!library) {
        setProblem(problemText(library.error()));
        return false;
    }
    setProblem({});
    m_canvas->commitWhole(updateGroup(m_canvas->document(), *library, group, *m_registry), tr("Update group"));
    return true;
}

void GroupEdit::requestGroupUpdate() {
    const QStringList ids = m_canvas->selectedIds();
    if (ids.size() == 1 && !m_canvas->updateAvailable(ids.first()).isEmpty())
        emit groupUpdateAsked(ids.first(), groupUpdate(ids.first()));
}
