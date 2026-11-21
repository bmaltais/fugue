#pragma once

// The node library (#56, ComfyUI's Node Library sidebar tab): every node type as a tree by
// category, the category levels being the `/`-separated parts of NodeType::category. All the logic
// is here: the tree, which levels are open, and the filter, which is node search's own ranking
// (searchScore), so a word finds the same nodes in both. src/panels/NodeLibraryPanel.qml only
// shows the rows; what a click or a drop does (add the node at a point of the canvas) is the
// window's, not this file's.

#include "engine/node_type.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <vector>

namespace fugue {

class Registry;

// One line of the library as shown.
struct LibraryRow {
    bool category = false;
    int depth = 0;       // 0 at the top; a node is one deeper than its category
    QString key;         // a category: its path ("Image/Transform"); a node: its type id
    QString label;       // a category's last part; a node's name
    int count = 0;       // a category: the nodes under it (all levels), that match the query
    bool open = false;   // a category: its children follow
    QString description; // a node's, for its tooltip
};

// The rows for `types`. Without a query: categories first (by name), then the nodes of a level
// by name, and a level's children only when its path is in `open`. With a query: only the types
// searchScore matches, every category on the way open, the nodes of a level best match first.
// Types with no category come last, at depth 0.
std::vector<LibraryRow> libraryRows(const QList<const NodeType *> &types, const QString &query,
                                    const QSet<QString> &open);

// libraryRows over the registry, with the open set and the query, as QML reads it.
class NodeLibrary : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // Maps: category, depth, key, label, count, open, description.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(int nodeCount READ nodeCount NOTIFY rowsChanged) // nodes shown

public:
    explicit NodeLibrary(const Registry *registry, QObject *parent = nullptr);

    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QVariantList rows() const { return m_rows; }
    int nodeCount() const { return m_nodes; }

    // Opens or closes a category by its key. No effect while a query shows every level open.
    Q_INVOKABLE void toggle(const QString &key);
    // The registry changed (a pack was installed or reloaded): the rows again.
    Q_INVOKABLE void refresh();

signals:
    void queryChanged();
    void rowsChanged();

private:
    const Registry *m_registry;
    QString m_query;
    QSet<QString> m_open;
    QVariantList m_rows;
    int m_nodes = 0;
};

} // namespace fugue
