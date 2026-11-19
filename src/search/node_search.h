#pragma once

// Node search (spec 12.8, 12.4): search-first node creation. This file holds all the logic
// (what is searched, how results rank, which are left out when a dragged link opened the
// search, what accepting a result does); src/search/NodeSearchPopup.qml only shows it, so
// the look can change without touching any of this.
//
// Search opens four ways: the `node.search` command (Tab, Shift+A) and a double-click on
// empty canvas open it at a world point; a link let go on empty canvas opens it filtered to
// the node types with a compatible port. Results show category, ports and version. Groups
// appear like any node type once they are registered as one (#26).
//
// Accepting a result adds the node through CanvasModel::addNode and, for a dragged link,
// connects it with CanvasModel::connectPorts, and nothing else touches the document, so
// #17 (undo) can wrap those two calls as one undoable command: it should wrap accept().

#include "engine/port_type.h"

#include <QAbstractListModel>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <optional>
#include <vector>

class CanvasModel;

namespace fugue {
class CommandRegistry;
class Registry;
struct NodeType;

// How well a node type matches a query: 0 for no match, higher is better. The query is
// split into words and every word must match the name, id, category, keywords, aliases or
// description (in that order of weight); a word matches as a prefix or inside a word, or
// loosely as letters in order (fuzzy). Case is ignored; an empty query matches everything.
int searchScore(const NodeType &type, const QString &query);

// Which port of the type a link from/to the dragged port would use, or an empty name when
// none is compatible. `draggedIsOutput`: the drag started on an output, so the new node must
// have a compatible input (the first, in declaration order); else it needs an output.
struct PortMatch {
    QString name;
    bool output = false;
    explicit operator bool() const { return !name.isEmpty(); }
};
PortMatch compatiblePort(const NodeType &type, const PortSpec &dragged, bool draggedIsOutput);

// The ranked results as a list model for QML.
class SearchResults : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")

public:
    enum Role {
        TypeId = Qt::UserRole + 1,
        Name,
        Category,
        Version,
        Description,
        Inputs,   // list of {name, label, type, color, shape, match}
        Outputs,
        MatchPort, // the port a dragged link would end on, or empty
    };

    explicit SearchResults(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    struct Row {
        const NodeType *type = nullptr;
        int score = 0;
        QString match; // see MatchPort
        bool matchOutput = false; // the match is an output (a link dragged from an input)
    };
    void set(std::vector<Row> rows);
    const Row *row(int index) const;

    // Set by NodeSearch, which knows the registry and the canvas.
    void setContext(const Registry *registry) { m_registry = registry; }

private:
    QVariantList portMaps(const NodeType &type, bool outputs, const QString &match) const;
    const Registry *m_registry = nullptr;
    std::vector<Row> m_rows;
};

class NodeSearch : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(SearchResults *results READ results CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY resultsChanged)
    // The dragged link this search was opened from, or empty: {node, port, output, type}.
    Q_PROPERTY(QVariantMap link READ link NOTIFY activeChanged)
    // One translated line saying what the results are filtered by, or empty; and the same as
    // a few words for the filter chip ("Takes image"), with the dragged port's color key and
    // shape (CanvasModel::colorKey, shape) in `link`.
    Q_PROPERTY(QString filterText READ filterText NOTIFY activeChanged)
    Q_PROPERTY(QString filterChip READ filterChip NOTIFY activeChanged)
    // How many node types there are to search, for "5 of 61".
    Q_PROPERTY(int total READ total NOTIFY resultsChanged)
    // Where the node goes, in world coordinates.
    Q_PROPERTY(QPointF point READ point NOTIFY activeChanged)

public:
    NodeSearch(const Registry *registry, CanvasModel *canvas, QObject *parent = nullptr);

    bool active() const { return m_active; }
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    SearchResults *results() { return &m_results; }
    int count() const { return m_results.rowCount(); }
    QVariantMap link() const { return m_link; }
    QString filterText() const { return m_filterText; }
    QString filterChip() const { return m_filterChip; }
    int total() const;
    QPointF point() const { return m_point; }

    // Opens search to add a node at the world point; the query starts empty.
    Q_INVOKABLE void openAt(qreal x, qreal y);
    // Opens it for a link let go at the world point; `from` is {node, port, output}, as the
    // canvas emits it. The results are the node types with a compatible port; when the
    // dragged port's type is not known (a placeholder) nothing is compatible.
    Q_INVOKABLE void openFromLink(const QVariantMap &from, qreal x, qreal y);
    Q_INVOKABLE void close();
    // Drops the dragged link's filter (the chip's ×): every node type shows, and accepting
    // one adds it unlinked.
    Q_INVOKABLE void clearFilter();

    // Adds the node of the result at `row` at the search point and, for a dragged link,
    // links it to the dragged port. Closes the search; returns the new node's id, or empty
    // when the row does not exist. A link the canvas then refuses leaves the node unlinked.
    Q_INVOKABLE QString accept(int row);

    // Makes `node.search` open search at the pointer: the canvas answers
    // pointerSearchRequested() with the pointer's world point (openAt).
    void registerCommands(CommandRegistry &commands);

signals:
    void activeChanged();
    void queryChanged();
    void resultsChanged();
    void pointerSearchRequested();

private:
    void refresh();

    const Registry *m_registry;
    CanvasModel *m_canvas;
    SearchResults m_results;
    bool m_active = false;
    QString m_query;
    QVariantMap m_link;
    std::optional<PortSpec> m_linkType;
    QString m_filterText;
    QString m_filterChip;
    QPointF m_point;
};

} // namespace fugue
