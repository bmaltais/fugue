#include "apps/app_builder.h"

#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

#include "canvas/canvas_model.h"
#include "engine/input_binding.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

const Registry &registryOr(const Registry *registry) {
    static const Registry none;
    return registry ? *registry : none;
}

// "negative_prompt" -> "Negative prompt": what an input with no label is called, as the form says it.
QString humanize(const QString &word) {
    QString text = word;
    text.replace(QLatin1Char('_'), QLatin1Char(' ')).replace(QLatin1Char('-'), QLatin1Char(' '));
    text = text.trimmed();
    return text.isEmpty() ? text : text.at(0).toUpper() + text.mid(1);
}

// A name an input of the form can have (a-z, 0-9, _; a letter first).
QString nameFrom(const QString &text) {
    QString name = text.toLower();
    name.replace(QRegularExpression(QStringLiteral("[^a-z0-9_]")), QStringLiteral("_"));
    if (name.isEmpty() || !name.at(0).isLetter())
        name.prepend(QLatin1Char('i'));
    return name;
}

// The role the templates give an input like this one, so the form draws the right control (an image well, a seed, a model
// picker); empty for a plain input.
QString roleFor(const InputSpec &spec, const Registry &registry) {
    const QString widget = spec.widget();
    if (widget == QLatin1String("file"))
        return QStringLiteral("image");
    if (widget == QLatin1String("mask"))
        return QStringLiteral("mask");
    if (widget == QLatin1String("seed"))
        return QStringLiteral("seed");
    if (registry.portKind(spec.type.type) == PortKind::Resource)
        return QStringLiteral("model");
    if (spec.type.type.id == QLatin1String("text") && !spec.type.list && !spec.choices) {
        if (spec.name == QLatin1String("prompt"))
            return QStringLiteral("prompt");
        if (spec.name == QLatin1String("negative_prompt"))
            return QStringLiteral("negative_prompt");
    }
    return {};
}

// The inputs a node has on its face (6.1.2): the declared ones, the rows of a repeating input that are there, and the key of the
// empty row after them ("Add LoRA"), which an app can fill to make that row. Not the other items of the empty row.
QList<const InputSpec *> faceInputs(const NodeType &type, const Node &node) {
    const auto has = [&node](const QString &name) { return !node.inputs.value(name).isEmpty(); };
    QStringList next;
    for (int r = 0; r < type.repeats.size(); ++r) {
        const QList<int> rows = type.rowsOf(r, has);
        const int row = rows.isEmpty() ? 1 : rows.last() + 1;
        if (row <= type.repeats.at(r).max)
            next << NodeType::rowInputName(type.repeats.at(r).items.first().name, row);
    }
    QList<const InputSpec *> list;
    for (const InputSpec *spec : type.inputsWhere([&](const QString &name) { return has(name) || next.contains(name); })) {
        const std::optional<RowInput> at = type.rowInput(spec->name);
        if (at && at->item > 0 && next.contains(NodeType::rowInputName(type.repeats.at(at->repeat).items.first().name, at->row)))
            continue;
        list << spec;
    }
    return list;
}

// What the row of the builder says the control will be.
QString kindFor(const InputBinding &input, const InputSpec *spec) {
    if (input.role == QLatin1String("image") || input.role == QLatin1String("mask") || input.role == QLatin1String("model")
        || input.role == QLatin1String("seed"))
        return input.role;
    if (input.choices && !input.choices->isEmpty())
        return QStringLiteral("choice");
    if (spec && spec->choices)
        return QStringLiteral("choice");
    if (spec && spec->type.type.id == QLatin1String("boolean") && !spec->type.list)
        return QStringLiteral("toggle");
    if (input.min || input.max || (spec && spec->type.type.id == QLatin1String("number") && !spec->type.list))
        return QStringLiteral("number");
    return QStringLiteral("text");
}

bool isSink(const Document &document, const Registry &registry, const QString &node) {
    const auto found = document.graph.nodes.constFind(node);
    if (found == document.graph.nodes.constEnd())
        return false;
    const NodeType *type = registry.nodeTypeFor(found->type, found->version.major);
    return type && type->sink;
}

// A literal as the text a field edits.
QString textOf(const QJsonValue &value) {
    if (value.isUndefined() || value.isNull())
        return {};
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble())
        return QString::number(value.toDouble(), 'g', 12);
    return value.toString();
}

bool wired(const Node &node, const QString &input) { return node.inputs.value(input).kind() == InputValue::Kind::Link; }

} // namespace

AppBuilder::AppBuilder(const Registry *registry, CanvasModel *canvas, QObject *parent)
    : QObject(parent), m_registry(registry), m_canvas(canvas) {}

bool AppBuilder::hasBlock() const { return m_canvas->document().extra.contains(QStringLiteral("app")); }

AppView *AppBuilder::preview() {
    if (!m_preview) {
        m_preview = std::make_unique<AppView>([](const RunRequest &) { return QString(); }, [](const QString &) {}, m_registry);
        m_preview->show(QStringLiteral("builder"), documentWithDraft(), {});
    }
    return m_preview.get();
}

Document AppBuilder::documentWithDraft() const {
    Document document = m_canvas->document();
    app::setApp(document, m_draft);
    return document;
}

void AppBuilder::setNotice(const QString &text) {
    if (m_notice == text)
        return;
    m_notice = text;
    emit noticeChanged();
}

void AppBuilder::touch() {
    if (m_preview)
        m_preview->show(QStringLiteral("builder"), documentWithDraft(), {});
    emit changed();
}

QString AppBuilder::titleOf(const QString &node) const {
    const QString title = m_canvas->nodeTitle(node);
    return title.isEmpty() ? node : title;
}

void AppBuilder::setTitle(const QString &title) {
    if (m_draft.title == title)
        return;
    m_draft.title = title;
    touch();
}

void AppBuilder::setDescription(const QString &description) {
    if (m_draft.description == description)
        return;
    m_draft.description = description;
    touch();
}

void AppBuilder::setDefaultView(const QString &view) {
    const QString wanted = view == QLatin1String("graph") ? QStringLiteral("graph") : QStringLiteral("app");
    if (m_draft.defaultView == wanted)
        return;
    m_draft.defaultView = wanted;
    touch();
}

int AppBuilder::indexOfInput(const QString &key) const {
    for (int i = 0; i < m_draft.inputs.size(); ++i)
        if (m_draft.inputs.at(i).key() == key)
            return i;
    return -1;
}

int AppBuilder::indexOfOutput(const QString &node) const {
    for (int i = 0; i < m_draft.outputs.size(); ++i)
        if (m_draft.outputs.at(i).node == node)
            return i;
    return -1;
}

// --- open and close -----------------------------------------------------------------------------

bool AppBuilder::begin(const QString &suggestedTitle) {
    if (m_canvas->inGroup()) {
        m_open = false;
        say(tr("Leave the group first: an app is made from the whole workflow."));
        emit stateChanged();
        return false;
    }
    const Document document = m_canvas->document();
    m_draft = {};
    m_editing = false;
    QString said;
    const Result<std::optional<app::App>> block = app::appOf(document);
    if (!block) {
        m_editing = true; // a block is there; applying replaces it
        said = tr("The workflow's app block cannot be read (%1), so the builder starts empty. Apply replaces it.")
                   .arg(block.errors().first().message);
    } else if (block->has_value()) {
        m_editing = true;
        m_draft = **block;
        const Errors dropped = app::pruneApp(m_draft, document, registryOr(m_registry));
        if (!dropped.isEmpty())
            said = tr("Some of this app's inputs or outputs are gone from the workflow; they are left out.");
    }
    if (m_draft.title.isEmpty())
        m_draft.title = suggestedTitle;
    m_open = true;
    m_picking = false;
    say(said);
    emit stateChanged();
    touch();
    return true;
}

void AppBuilder::cancel() {
    if (!m_open)
        return;
    m_open = false;
    m_picking = false;
    say({});
    emit stateChanged();
}

bool AppBuilder::apply(bool openAppView) {
    if (!m_open || !problems().isEmpty())
        return false;
    return writeBlock(false, openAppView);
}

bool AppBuilder::removeApp() {
    if (!m_open || !m_editing)
        return false;
    return writeBlock(true, false);
}

bool AppBuilder::writeBlock(bool remove, bool openAppView) {
    Document document = m_canvas->document();
    if (remove)
        app::clearApp(document);
    else
        app::setApp(document, m_draft);
    m_canvas->commitWhole(document, remove ? tr("Remove app") : m_editing ? tr("Edit app") : tr("Make app"));
    m_open = false;
    m_picking = false;
    say({});
    emit stateChanged();
    emit applied(openAppView && !remove);
    return true;
}

// --- reading --------------------------------------------------------------------------------------

QVariantList AppBuilder::inputs() const {
    QVariantList rows;
    const Document document = m_canvas->document();
    const Registry &registry = registryOr(m_registry);
    for (int i = 0; i < m_draft.inputs.size(); ++i) {
        const InputBinding &input = m_draft.inputs.at(i);
        const InputSpec *spec = input.targets.isEmpty() ? nullptr : targetInputSpec(document, registry, input.targets.first());
        const InputBinding full = completedBy(input, spec);
        QStringList targets;
        QStringList titles;
        for (const PortRef &target : input.targets) {
            targets << target.toString();
            titles << titleOf(target.node);
        }
        const QString kind = kindFor(input, spec);
        QVariantMap row{{"key", input.key()},
                        {"label", input.label},
                        {"shown", input.label.isEmpty() ? humanize(input.key()) : input.label},
                        {"role", input.role},
                        {"kind", kind},
                        {"type", spec ? spec->type.toString() : QString()},
                        {"section", input.section},
                        {"advanced", input.section == QLatin1String("Advanced")},
                        {"ranged", kind == QLatin1String("number")},
                        {"default", textOf(input.defaultValue)},
                        {"targets", targets},
                        {"titles", titles},
                        {"first", i == 0},
                        {"last", i == m_draft.inputs.size() - 1}};
        // What the form would offer when the block says nothing: the target's own range, as the placeholder.
        row.insert(QStringLiteral("min"), input.min ? QVariant(textOf(*input.min)) : QVariant(QString()));
        row.insert(QStringLiteral("max"), input.max ? QVariant(textOf(*input.max)) : QVariant(QString()));
        row.insert(QStringLiteral("innerMin"), full.min ? textOf(*full.min) : QString());
        row.insert(QStringLiteral("innerMax"), full.max ? textOf(*full.max) : QString());
        rows << row;
    }
    return rows;
}

QVariantList AppBuilder::outputs() const {
    QVariantList rows;
    const Document document = m_canvas->document();
    const Registry &registry = registryOr(m_registry);
    for (int i = 0; i < m_draft.outputs.size(); ++i) {
        const app::AppOutput &output = m_draft.outputs.at(i);
        const auto node = document.graph.nodes.constFind(output.node);
        const NodeType *type = node == document.graph.nodes.constEnd() ? nullptr : registry.nodeTypeFor(node->type, node->version.major);
        rows << QVariantMap{{"node", output.node},
                            {"title", titleOf(output.node)},
                            {"label", output.label},
                            {"type", type ? type->name : QString()},
                            {"sink", type && type->sink},
                            {"first", i == 0},
                            {"last", i == m_draft.outputs.size() - 1}};
    }
    return rows;
}

QVariantList AppBuilder::marks() const {
    QVariantList marks;
    for (const InputBinding &input : m_draft.inputs)
        for (const PortRef &target : input.targets) {
            const QRectF at = m_canvas->inputRowRect(target.node, target.port);
            if (!at.isEmpty())
                marks << QVariantMap{{"x", at.x()}, {"y", at.y()}, {"width", at.width()}, {"height", at.height()}, {"output", false}};
        }
    for (const app::AppOutput &output : m_draft.outputs) {
        const QRectF at = m_canvas->nodeRect(output.node);
        if (!at.isEmpty())
            marks << QVariantMap{{"x", at.x()}, {"y", at.y()}, {"width", at.width()}, {"height", at.height()}, {"output", true}};
    }
    return marks;
}

QStringList AppBuilder::problems() const {
    QStringList found;
    if (m_draft.title.trimmed().isEmpty())
        found << tr("Give the app a title.");
    if (m_draft.inputs.isEmpty() && m_draft.outputs.isEmpty())
        found << tr("An app needs something to show: pick an input to change and an output (a Save or Preview node).");
    else if (m_draft.outputs.isEmpty())
        found << tr("Pick at least one output: the Save or Preview node whose result the app shows.");
    const Errors errors = app::checkApp(m_draft, m_canvas->document(), registryOr(m_registry));
    for (const Error &error : errors) {
        if (error.message.startsWith(QLatin1String("The app has no output")))
            continue; // said above, in the builder's words
        found << error.message;
    }
    return found;
}

// --- the inputs -------------------------------------------------------------------------------------

bool AppBuilder::exposeInput(const QString &node, const QString &input) {
    const Document document = m_canvas->document();
    const Registry &registry = registryOr(m_registry);
    const auto found = document.graph.nodes.constFind(node);
    if (found == document.graph.nodes.constEnd()) {
        say(tr("The workflow has no node %1.").arg(node));
        return false;
    }
    const PortRef target{node, input};
    const InputSpec *spec = targetInputSpec(document, registry, target);
    if (!spec) {
        say(tr("%1 has no input %2.").arg(titleOf(node), input));
        return false;
    }
    const QString shown = spec->label.isEmpty() ? humanize(input) : spec->label;
    for (const InputBinding &each : m_draft.inputs)
        if (each.targets.contains(target)) {
            say(tr("%1 is already in the app.").arg(shown));
            return false;
        }
    if (wired(*found, input)) {
        say(tr("%1 is linked to another node, so it has no value to set. Cut the link first.").arg(shown));
        return false;
    }
    if (spec->widget().isEmpty() && registry.portKind(spec->type.type) != PortKind::Resource) {
        say(tr("%1 only takes a connection, not a value, so the form cannot ask for it.").arg(shown));
        return false;
    }
    InputBinding binding;
    binding.role = roleFor(*spec, registry);
    QSet<QString> keys;
    for (const InputBinding &each : m_draft.inputs)
        keys.insert(each.key());
    if (binding.role.isEmpty() || keys.contains(binding.role)) {
        // A plain input is keyed by its name; a role taken already (a second picture) keeps its role with a name of its own.
        QString name = nameFrom(input);
        if (keys.contains(name))
            name = nameFrom(node + QLatin1Char('_') + input);
        const QString base = name;
        for (int n = 2; keys.contains(name); ++n)
            name = base + QLatin1Char('_') + QString::number(n);
        binding.name = name;
    }
    binding.targets << target;
    binding.label = binding.role == QLatin1String("image") ? tr("Picture") : spec->label; // the picture, not "File"
    m_draft.inputs << binding;
    say(tr("Added %1.").arg(shown));
    touch();
    return true;
}

bool AppBuilder::toggleInput(const QString &node, const QString &input) {
    const PortRef target{node, input};
    for (int i = 0; i < m_draft.inputs.size(); ++i) {
        InputBinding &each = m_draft.inputs[i];
        if (!each.targets.contains(target))
            continue;
        const QString shown = each.label.isEmpty() ? humanize(each.key()) : each.label;
        each.targets.removeAll(target);
        if (each.targets.isEmpty())
            m_draft.inputs.removeAt(i);
        say(tr("Stopped exposing %1.").arg(shown));
        touch();
        return true;
    }
    return exposeInput(node, input);
}

bool AppBuilder::removeInput(const QString &key) {
    const int at = indexOfInput(key);
    if (at < 0)
        return false;
    m_draft.inputs.removeAt(at);
    touch();
    return true;
}

bool AppBuilder::editInput(const QString &key, const QVariantMap &change) {
    const int at = indexOfInput(key);
    if (at < 0)
        return false;
    InputBinding &input = m_draft.inputs[at];
    const Document document = m_canvas->document();
    const InputSpec *spec = input.targets.isEmpty() ? nullptr : targetInputSpec(document, registryOr(m_registry), input.targets.first());
    const auto number = [this](const QVariant &text, std::optional<double> &into) {
        const QString word = text.toString().trimmed();
        if (word.isEmpty()) {
            into.reset();
            return true;
        }
        bool ok = false;
        const double value = word.toDouble(&ok);
        if (!ok) {
            say(tr("\"%1\" is not a number.").arg(word));
            return false;
        }
        into = value;
        return true;
    };
    bool ok = true;
    if (change.contains(QStringLiteral("label")))
        input.label = change.value(QStringLiteral("label")).toString().trimmed();
    if (change.contains(QStringLiteral("advanced"))) {
        if (change.value(QStringLiteral("advanced")).toBool())
            input.section = QStringLiteral("Advanced");
        else if (input.section == QLatin1String("Advanced"))
            input.section.clear();
    }
    if (change.contains(QStringLiteral("min")))
        ok = number(change.value(QStringLiteral("min")), input.min) && ok;
    if (change.contains(QStringLiteral("max")))
        ok = number(change.value(QStringLiteral("max")), input.max) && ok;
    if (change.contains(QStringLiteral("default"))) {
        const QString word = change.value(QStringLiteral("default")).toString();
        const QString type = spec ? spec->type.type.id : QString();
        if (word.trimmed().isEmpty()) {
            input.defaultValue = QJsonValue::Undefined;
        } else if (type == QLatin1String("number")) {
            bool numeric = false;
            const double value = word.toDouble(&numeric);
            if (numeric)
                input.defaultValue = value;
            else {
                say(tr("\"%1\" is not a number.").arg(word.trimmed()));
                ok = false;
            }
        } else if (type == QLatin1String("boolean")) {
            input.defaultValue = word.trimmed().toLower() == QLatin1String("true");
        } else {
            input.defaultValue = word;
        }
    }
    touch();
    return ok;
}

bool AppBuilder::moveInput(const QString &key, int to) {
    const int at = indexOfInput(key);
    if (at < 0 || to < 0 || to >= m_draft.inputs.size())
        return false;
    if (at != to) {
        m_draft.inputs.move(at, to);
        touch();
    }
    return true;
}

QVariantList AppBuilder::driveCandidates(const QString &key) const {
    QVariantList list;
    const int at = indexOfInput(key);
    if (at < 0 || m_draft.inputs.at(at).targets.isEmpty())
        return list;
    const Document document = m_canvas->document();
    const Registry &registry = registryOr(m_registry);
    const InputSpec *first = targetInputSpec(document, registry, m_draft.inputs.at(at).targets.first());
    if (!first)
        return list;
    QSet<QString> taken;
    for (const InputBinding &each : m_draft.inputs)
        for (const PortRef &target : each.targets)
            taken.insert(target.toString());
    for (auto node = document.graph.nodes.cbegin(); node != document.graph.nodes.cend(); ++node) {
        const NodeType *type = registry.nodeTypeFor(node->type, node->version.major);
        if (!type)
            continue;
        for (const InputSpec *each : faceInputs(*type, *node)) {
            const InputSpec &spec = *each;
            if (spec.type.type != first->type.type || spec.type.list != first->type.list || taken.contains(node.key() + QLatin1Char('.') + spec.name)
                || wired(*node, spec.name) || (spec.widget().isEmpty() && registry.portKind(spec.type.type) != PortKind::Resource))
                continue;
            list << QVariantMap{{"node", node.key()}, {"input", spec.name}, {"title", titleOf(node.key())},
                                {"label", spec.label.isEmpty() ? humanize(spec.name) : spec.label}};
        }
    }
    return list;
}

bool AppBuilder::alsoDrive(const QString &key, const QString &node, const QString &input) {
    const int at = indexOfInput(key);
    if (at < 0)
        return false;
    const Document document = m_canvas->document();
    const InputSpec *spec = targetInputSpec(document, registryOr(m_registry), {node, input});
    const InputSpec *first = m_draft.inputs.at(at).targets.isEmpty()
                                 ? nullptr
                                 : targetInputSpec(document, registryOr(m_registry), m_draft.inputs.at(at).targets.first());
    if (!spec || !first) {
        say(tr("%1 has no input %2.").arg(titleOf(node), input));
        return false;
    }
    if (spec->type.type != first->type.type || spec->type.list != first->type.list) {
        say(tr("%1 takes %2, not %3, so one input cannot drive both.").arg(spec->label.isEmpty() ? humanize(input) : spec->label,
                                                                          spec->type.toString(), first->type.toString()));
        return false;
    }
    for (const InputBinding &each : m_draft.inputs)
        if (each.targets.contains({node, input})) {
            say(tr("%1 is already in the app.").arg(spec->label.isEmpty() ? humanize(input) : spec->label));
            return false;
        }
    if (wired(document.graph.nodes.value(node), input)) {
        say(tr("%1 is linked to another node, so it has no value to set. Cut the link first.").arg(spec->label.isEmpty() ? humanize(input) : spec->label));
        return false;
    }
    m_draft.inputs[at].targets << PortRef{node, input};
    touch();
    return true;
}

QVariantList AppBuilder::inputChoices() const {
    struct Choice {
        QString node, input, title, label;
        bool exposed;
    };
    QList<Choice> choices;
    const Document document = m_canvas->document();
    const Registry &registry = registryOr(m_registry);
    QSet<QString> taken;
    for (const InputBinding &each : m_draft.inputs)
        for (const PortRef &target : each.targets)
            taken.insert(target.toString());
    for (auto node = document.graph.nodes.cbegin(); node != document.graph.nodes.cend(); ++node) {
        const NodeType *type = registry.nodeTypeFor(node->type, node->version.major);
        if (!type)
            continue;
        for (const InputSpec *each : faceInputs(*type, *node)) {
            const InputSpec &spec = *each;
            if (wired(*node, spec.name) || (spec.widget().isEmpty() && registry.portKind(spec.type.type) != PortKind::Resource))
                continue;
            choices << Choice{node.key(), spec.name, titleOf(node.key()), spec.label.isEmpty() ? humanize(spec.name) : spec.label,
                              taken.contains(node.key() + QLatin1Char('.') + spec.name)};
        }
    }
    std::stable_sort(choices.begin(), choices.end(), [](const Choice &a, const Choice &b) { return a.title.localeAwareCompare(b.title) < 0; });
    QVariantList list;
    for (const Choice &choice : std::as_const(choices))
        list << QVariantMap{{"node", choice.node}, {"input", choice.input}, {"title", choice.title}, {"label", choice.label},
                            {"exposed", choice.exposed}};
    return list;
}

// --- the outputs ------------------------------------------------------------------------------------

bool AppBuilder::addOutput(const QString &node) {
    const Document document = m_canvas->document();
    if (!document.graph.nodes.contains(node)) {
        say(tr("The workflow has no node %1.").arg(node));
        return false;
    }
    if (indexOfOutput(node) >= 0) {
        say(tr("%1 is already an output.").arg(titleOf(node)));
        return false;
    }
    m_draft.outputs << app::AppOutput{node, {}};
    say(tr("Added the result of %1.").arg(titleOf(node)));
    touch();
    return true;
}

bool AppBuilder::toggleOutput(const QString &node) {
    if (indexOfOutput(node) < 0)
        return addOutput(node);
    removeOutput(node);
    say(tr("Stopped showing the result of %1.").arg(titleOf(node)));
    return true;
}

bool AppBuilder::removeOutput(const QString &node) {
    const int at = indexOfOutput(node);
    if (at < 0)
        return false;
    m_draft.outputs.removeAt(at);
    touch();
    return true;
}

bool AppBuilder::setOutputLabel(const QString &node, const QString &label) {
    const int at = indexOfOutput(node);
    if (at < 0)
        return false;
    m_draft.outputs[at].label = label.trimmed();
    touch();
    return true;
}

bool AppBuilder::moveOutput(const QString &node, int to) {
    const int at = indexOfOutput(node);
    if (at < 0 || to < 0 || to >= m_draft.outputs.size())
        return false;
    if (at != to) {
        m_draft.outputs.move(at, to);
        touch();
    }
    return true;
}

QVariantList AppBuilder::outputChoices() const {
    QVariantList list;
    const Document document = m_canvas->document();
    const Registry &registry = registryOr(m_registry);
    for (auto node = document.graph.nodes.cbegin(); node != document.graph.nodes.cend(); ++node)
        if (isSink(document, registry, node.key()))
            list << QVariantMap{{"node", node.key()}, {"title", titleOf(node.key())}, {"added", indexOfOutput(node.key()) >= 0}};
    std::stable_sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("title").toString().localeAwareCompare(b.toMap().value("title").toString()) < 0;
    });
    return list;
}

// --- click-to-expose ------------------------------------------------------------------------------------

void AppBuilder::startPicking() {
    if (!m_open || m_picking)
        return;
    m_picking = true;
    say(tr("Click an input to add it to the app, a Save or Preview node to show its result."));
    emit stateChanged();
}

void AppBuilder::finishPicking() {
    if (!m_picking)
        return;
    m_picking = false;
    emit stateChanged();
}

void AppBuilder::pickAt(qreal x, qreal y) {
    if (!m_open || !m_picking)
        return;
    const QVariantMap row = m_canvas->inputRowAt(x, y);
    if (!row.isEmpty()) {
        toggleInput(row.value(QStringLiteral("node")).toString(), row.value(QStringLiteral("input")).toString());
        return;
    }
    const QString node = m_canvas->nodeAt(x, y);
    if (node.isEmpty())
        return;
    if (isSink(m_canvas->document(), registryOr(m_registry), node))
        toggleOutput(node);
    else
        say(tr("%1 has no result to show. Click one of its inputs, or a Save or Preview node.").arg(titleOf(node)));
}
