#include "search/node_search.h"

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/registry.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>

namespace fugue {

namespace {

QStringList wordsOf(const QString &text) {
    return text.toLower().split(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), Qt::SkipEmptyParts);
}

// The letters of `word` appear in `text` in order.
bool isSubsequence(const QString &word, const QString &text) {
    qsizetype at = 0;
    for (const QChar c : text)
        if (at < word.size() && c == word[at])
            ++at;
    return at == word.size();
}

bool startsAWord(const QString &text, const QString &word) {
    for (const QString &w : wordsOf(text))
        if (w.startsWith(word))
            return true;
    return false;
}

int scoreWord(const NodeType &type, const QString &word) {
    int best = 0;
    auto offer = [&best](int score) { best = std::max(best, score); };
    const QString name = type.name.toLower();
    if (name == word)
        offer(100);
    else if (name.startsWith(word))
        offer(80);
    else if (startsAWord(name, word))
        offer(60);
    else if (name.contains(word))
        offer(45);
    const QString id = type.id.toLower();
    const QString shortId = id.section('/', -1);
    if (shortId.startsWith(word))
        offer(40);
    else if (id.contains(word))
        offer(25);
    for (const QString &keyword : type.keywords) {
        const QString k = keyword.toLower();
        if (k == word)
            offer(50);
        else if (k.startsWith(word))
            offer(35);
        else if (k.contains(word))
            offer(20);
    }
    for (const QString &alias : type.aliases)
        if (alias.toLower().contains(word))
            offer(15);
    const QString category = type.category.toLower();
    if (startsAWord(category, word))
        offer(30);
    else if (category.contains(word))
        offer(25);
    if (type.description.toLower().contains(word))
        offer(10);
    if (best == 0 && word.size() >= 2 && isSubsequence(word, name))
        offer(12);
    return best;
}

QString versionText(const NodeType &type) { return type.version.toString(); }

} // namespace

int searchScore(const NodeType &type, const QString &query) {
    const QStringList words = wordsOf(query);
    if (words.isEmpty())
        return 1;
    int total = 0;
    for (const QString &word : words) {
        const int score = scoreWord(type, word);
        if (score == 0)
            return 0;
        total += score;
    }
    return total;
}

PortMatch compatiblePort(const NodeType &type, const PortSpec &dragged, bool draggedIsOutput) {
    if (draggedIsOutput) {
        for (const InputSpec &in : type.inputs)
            if (compatibility(dragged, in.type) == Compatibility::Compatible)
                return {in.name, false};
    } else {
        for (const OutputSpec &out : type.outputs)
            if (compatibility(out.type, dragged) == Compatibility::Compatible)
                return {out.name, true};
    }
    return {};
}

// --- results -----------------------------------------------------------------

int SearchResults::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> SearchResults::roleNames() const {
    return {{TypeId, "typeId"},   {Name, "name"},       {Category, "category"},
            {Version, "version"}, {Description, "description"}, {Inputs, "inputs"},
            {Outputs, "outputs"}, {MatchPort, "matchPort"}};
}

const SearchResults::Row *SearchResults::row(int index) const {
    return index >= 0 && index < int(m_rows.size()) ? &m_rows[size_t(index)] : nullptr;
}

void SearchResults::set(std::vector<Row> rows) {
    // Rows removed and inserted, not a model reset: a reset while the list is still making
    // delegates for the last query makes Qt warn.
    if (!m_rows.empty()) {
        beginRemoveRows({}, 0, int(m_rows.size()) - 1);
        m_rows.clear();
        endRemoveRows();
    }
    if (!rows.empty()) {
        beginInsertRows({}, 0, int(rows.size()) - 1);
        m_rows = std::move(rows);
        endInsertRows();
    }
}

QVariantList SearchResults::portMaps(const NodeType &type, bool outputs, const QString &match) const {
    QVariantList list;
    auto add = [&](const QString &name, const QString &label, const PortSpec &spec) {
        const std::optional<PortSpec> known = spec;
        const PortKind kind = m_registry ? m_registry->portKind(spec.type) : PortKind::Unknown;
        list.append(QVariantMap{{"name", name},
                                {"label", label.isEmpty() ? name : label},
                                {"type", spec.toString()},
                                {"color", CanvasModel::colorKey(known, kind)},
                                {"shape", CanvasModel::shape(known, kind)},
                                {"match", !match.isEmpty() && name == match}});
    };
    if (outputs)
        for (const OutputSpec &out : type.outputs)
            add(out.name, out.label, out.type);
    else
        for (const InputSpec &in : type.inputs)
            add(in.name, in.label, in.type);
    return list;
}

QVariant SearchResults::data(const QModelIndex &index, int role) const {
    const Row *r = index.isValid() ? row(index.row()) : nullptr;
    if (!r)
        return {};
    const NodeType &type = *r->type;
    switch (role) {
    case TypeId: return type.id;
    case Name: return type.name;
    case Category: return type.category;
    case Version: return versionText(type);
    case Description: return type.description;
    // `match` marks the port a link would end on: an input when dragged from an output.
    case Inputs: return portMaps(type, false, r->matchOutput ? QString() : r->match);
    case Outputs: return portMaps(type, true, r->matchOutput ? r->match : QString());
    case MatchPort: return r->match;
    }
    return {};
}

// --- search ------------------------------------------------------------------

NodeSearch::NodeSearch(const Registry *registry, CanvasModel *canvas, QObject *parent)
    : QObject(parent), m_registry(registry), m_canvas(canvas) {
    m_results.setContext(registry);
}

void NodeSearch::setQuery(const QString &query) {
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    refresh();
}

void NodeSearch::refresh() {
    std::vector<SearchResults::Row> rows;
    if (m_registry && !(m_link.contains("node") && !m_linkType)) {
        const bool draggedIsOutput = m_link.value("output").toBool();
        for (const NodeType *type : m_registry->nodeTypes()) {
            SearchResults::Row row;
            row.type = type;
            if (m_linkType) {
                const PortMatch match = compatiblePort(*type, *m_linkType, draggedIsOutput);
                if (!match)
                    continue;
                row.match = match.name;
                row.matchOutput = match.output;
            }
            row.score = searchScore(*type, m_query);
            if (row.score > 0)
                rows.push_back(row);
        }
    }
    const bool ranked = !wordsOf(m_query).isEmpty();
    std::sort(rows.begin(), rows.end(), [ranked](const SearchResults::Row &a, const SearchResults::Row &b) {
        if (ranked && a.score != b.score)
            return a.score > b.score;
        if (!ranked && a.type->category != b.type->category)
            return a.type->category < b.type->category;
        if (ranked && a.type->name.size() != b.type->name.size())
            return a.type->name.size() < b.type->name.size();
        if (a.type->name != b.type->name)
            return a.type->name < b.type->name;
        return a.type->id < b.type->id;
    });
    m_results.set(std::move(rows));
    emit resultsChanged();
}

void NodeSearch::openAt(qreal x, qreal y) {
    m_point = {x, y};
    m_link.clear();
    m_linkType.reset();
    m_filterText.clear();
    m_query.clear();
    m_active = true;
    refresh();
    emit queryChanged();
    emit activeChanged();
}

void NodeSearch::openFromLink(const QVariantMap &from, qreal x, qreal y) {
    const QString node = from.value("node").toString();
    const QString port = from.value("port").toString();
    const bool output = from.value("output").toBool();
    m_point = {x, y};
    m_linkType = m_canvas ? m_canvas->portType(node, port, output) : std::nullopt;
    m_link = {{"node", node}, {"port", port}, {"output", output},
              {"type", m_linkType ? m_linkType->toString() : QString()}};
    if (!m_linkType)
        m_filterText = QCoreApplication::translate("NodeSearch", "The type of that port is not known, so no node fits it.");
    else if (output)
        m_filterText = QCoreApplication::translate("NodeSearch", "Nodes with an input that takes %1").arg(m_linkType->toString());
    else
        m_filterText = QCoreApplication::translate("NodeSearch", "Nodes with an output that gives %1").arg(m_linkType->toString());
    m_query.clear();
    m_active = true;
    refresh();
    emit queryChanged();
    emit activeChanged();
}

void NodeSearch::close() {
    if (!m_active)
        return;
    m_active = false;
    emit activeChanged();
}

QString NodeSearch::accept(int row) {
    const SearchResults::Row *chosen = m_results.row(row);
    if (!chosen || !m_canvas)
        return {};
    const bool linked = m_link.contains("node") && !chosen->match.isEmpty();
    const bool fromOutput = m_link.value("output").toBool();
    // A node dropped from an input goes to the left of the point, so its output faces it.
    const qreal x = linked && !fromOutput ? m_point.x() - CanvasModel::defaultWidth : m_point.x();
    // The two calls #17 wraps as one command.
    const QString id = m_canvas->addNode(chosen->type->id, x, m_point.y());
    if (!id.isEmpty()) {
        if (linked) {
            const QString node = m_link.value("node").toString(), port = m_link.value("port").toString();
            if (fromOutput)
                m_canvas->connectPorts(node, port, id, chosen->match);
            else
                m_canvas->connectPorts(id, chosen->match, node, port);
        }
        m_canvas->select({id});
    }
    close();
    return id;
}

void NodeSearch::registerCommands(CommandRegistry &commands) {
    commands.setHandler(QStringLiteral("node.search"), [this] { emit pointerSearchRequested(); });
}

} // namespace fugue
