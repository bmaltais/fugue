#include "search/node_library.h"

#include <QCollator>
#include <QMap>
#include <QVariantMap>

#include <algorithm>

#include "engine/registry.h"
#include "search/node_search.h"

namespace fugue {

namespace {

struct Scored {
    const NodeType *type;
    int score;
};

// A level of the tree: its children levels by name, and the nodes directly in it.
struct Level {
    QMap<QString, Level> children;
    std::vector<Scored> nodes;
    int total = 0; // nodes in this level and below
};

// A group the user saved to the library: a group type of the reserved `local` pack (spec 5.3). A
// pack's own groups, and every other node, are not: a pack is removed from Settings > Packs.
bool isLibraryGroup(const NodeType &type) {
    return type.id.startsWith(QLatin1String("local/")) && type.runtime.value(QLatin1String("kind")).toString() == QLatin1String("group");
}

void appendLevel(const Level &level, const QString &path, int depth, bool query, const QSet<QString> &open, const QCollator &collator,
          std::vector<LibraryRow> &out) {
    QStringList names = level.children.keys();
    std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    for (const QString &name : names) {
        const Level &child = level.children[name];
        const QString key = path.isEmpty() ? name : path + QLatin1Char('/') + name;
        LibraryRow row;
        row.category = true;
        row.depth = depth;
        row.key = key;
        row.label = name;
        row.count = child.total;
        row.open = query || open.contains(key);
        out.push_back(row);
        if (row.open)
            appendLevel(child, key, depth + 1, query, open, collator, out);
    }
    std::vector<Scored> nodes = level.nodes;
    std::stable_sort(nodes.begin(), nodes.end(), [&](const Scored &a, const Scored &b) {
        if (query && a.score != b.score)
            return a.score > b.score;
        return collator.compare(a.type->name, b.type->name) < 0;
    });
    for (const Scored &node : nodes) {
        LibraryRow row;
        row.depth = depth;
        row.key = node.type->id;
        row.label = node.type->name;
        row.description = node.type->description;
        row.library = isLibraryGroup(*node.type);
        out.push_back(row);
    }
}

} // namespace

std::vector<LibraryRow> libraryRows(const QList<const NodeType *> &types, const QString &query, const QSet<QString> &open) {
    const bool filtered = !query.trimmed().isEmpty();
    Level root;
    for (const NodeType *type : types) {
        const int score = filtered ? searchScore(*type, query) : 1;
        if (score <= 0)
            continue;
        Level *level = &root;
        ++level->total;
        for (const QString &part : type->category.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            level = &level->children[part.trimmed()];
            ++level->total;
        }
        level->nodes.push_back({type, score});
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    std::vector<LibraryRow> out;
    // The nodes with no category come after every category, so they are emitted last by appendLevel()
    // itself (a level's nodes follow its children).
    appendLevel(root, QString(), 0, filtered, open, collator, out);
    return out;
}

NodeLibrary::NodeLibrary(const Registry *registry, QObject *parent) : QObject(parent), m_registry(registry) { refresh(); }

void NodeLibrary::setQuery(const QString &query) {
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    refresh();
}

void NodeLibrary::toggle(const QString &key) {
    if (!m_open.remove(key))
        m_open.insert(key);
    refresh();
}

void NodeLibrary::refresh() {
    m_rows.clear();
    m_nodes = 0;
    if (m_registry) {
        for (const LibraryRow &row : libraryRows(m_registry->nodeTypes(), m_query, m_open)) {
            if (!row.category)
                ++m_nodes;
            m_rows.append(QVariantMap{{"category", row.category},
                                      {"depth", row.depth},
                                      {"key", row.key},
                                      {"label", row.label},
                                      {"count", row.count},
                                      {"open", row.open},
                                      {"description", row.description},
                                      {"library", row.library}});
        }
    }
    emit rowsChanged();
}

} // namespace fugue
