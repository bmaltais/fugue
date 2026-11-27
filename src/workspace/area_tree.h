#pragma once

// The area tree (#217, spec 12.16, docs/workspaces-research.md 5.1): how the window's body is divided into
// areas, each showing one view (the Graph today; later the Canvas editor, the Mask editor, Queue, Results...).
// A split tree, as VS Code's grid and Blender's screen: a *leaf* is an area {id, view type, state per view type};
// a *branch* lays two or more children out along one axis, each with its share of the branch. Shares are
// fractions summing to 1, never pixels, so a window resize scales every area in proportion. Plain C++ with no
// window and no QML: the rules are tested alone (tests/workspace/test_area_tree.cpp); AreaNode.qml draws a tree.
//
// Axis: a `Row` branch lays its children from the start edge to the end edge (left to right, and right to left
// in a right-to-left language), a `Column` branch from top to bottom. Never left and right: a saved layout reads
// the same in every language, and only drawing mirrors it.
//
// An area keeps a state per view type it has shown (Blender's `spacedata`): switching an area from Graph to
// another view and back finds the Graph's state (its camera) again. The state is the area's, the data is the
// app's: two areas look at the same project.
//
// Saved form, format 1 (written only once a layout changes: the default tree is built in and never stored):
//   {"format": 1, "root": NODE}
//   NODE = {"type": "leaf", "area": 1, "view": "graph", "state": {"graph": {...}}}
//        | {"type": "branch", "axis": "row" | "column", "children": [{"size": 0.62, "node": NODE}, ...]}
// Keys this version does not know are kept on the node and written back, and so is a view type no view
// registers (the window shows a placeholder for it, workspace/view_registry.h), so an older or newer build
// loses nothing. A tree that does not validate (not this format, a node that is neither a leaf nor a branch, an
// area id used twice, a size that is not a positive number, deeper than kMaxDepth, more than kMaxAreas) gives
// the default tree. Redundant shapes are normalized, not refused: a branch with one child is that child, a
// branch inside a branch of the same axis is merged into it, shares are scaled to sum to 1.

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <functional>
#include <vector>

namespace fugue {

enum class AreaAxis { Row, Column };

struct Area {
    int id = 0;
    QString view;      // the view type id ("graph")
    QJsonObject state; // view type id -> that view's state in this area
};

class AreaTree {
public:
    static constexpr int kFormat = 1;
    static constexpr int kMaxDepth = 16;
    static constexpr int kMaxAreas = 64;

    struct Node {
        bool leaf = true;
        Area area;                     // a leaf's
        AreaAxis axis = AreaAxis::Row; // a branch's
        std::vector<Node> children;    // a branch's, two or more
        double size = 1;               // the share of its parent branch; 1 for the root
        QJsonObject extra;             // keys of the node this version does not know
    };
    // The minimum size of a view type, in pixels (ViewRegistry::minimumSize).
    using MinimumSize = std::function<QSizeF(const QString &view)>;
    // A node by the child indexes from the root: {} is the root, {1, 0} the first child of its second child.
    using Path = QList<int>;
    struct Placed {
        int area = 0;
        QRectF rect;
    };

    // The default tree: one area, id 1, showing the Graph.
    AreaTree();

    const Node &root() const { return m_root; }
    // The areas in reading order (depth first, start to end, top to bottom).
    QList<int> areaIds() const;
    int count() const { return int(areaIds().size()); }
    const Area *area(int id) const;
    // The path of the leaf of area `id`, or empty with *found false.
    Path pathOf(int id, bool *found = nullptr) const;
    const Node *node(const Path &path) const;

    // Splits area `id` along `axis`: a new area showing `view` takes `share` of its place, after it (at the end
    // or bottom) or before it. Inside a branch of the same axis the new area becomes a sibling, else the area
    // becomes a branch of two. The new area's id, or 0 when nothing changed (no such area, a share outside
    // (0, 1), an empty view, kMaxAreas reached, deeper than kMaxDepth). Whether the result fits a size is `fits`.
    int split(int id, AreaAxis axis, const QString &view, double share = 0.5, bool atEnd = true);
    // Area `gone` gives its place to `keep`: two leaves side by side in one branch. False when they are not.
    bool join(int keep, int gone);
    // Area `id` goes and its share goes to the child beside it in its branch: the one before it, or after it
    // when it is the first (Blender's Close Area). The area that now holds that place's start, the first area in
    // reading order of that neighbour, or 0 when nothing changed (no such area, or the only one).
    int close(int id);
    // Areas `a` and `b` trade what they show: the view and the state of every view they showed. Their places
    // and ids stay. False when either is missing or they are the same.
    bool swap(int a, int b);
    // The two children beside border `border` of the branch at `path` share their place equally. False when
    // nothing changed.
    bool equalize(const Path &path, int border);
    // Moves the border after child `border` of the branch at `path` by `delta` pixels, for a branch `length`
    // pixels long along its axis. Neither neighbour goes below its minimum; false when nothing moved.
    bool resize(const Path &path, int border, double delta, double length, const MinimumSize &minimum);
    // Area `id` shows `view` now; the state of the view it showed is kept, and the new one's found again.
    bool setView(int id, const QString &view);
    // The state of area `id`'s current view.
    bool setState(int id, const QJsonObject &state);

    // The smallest size the tree can be laid out in: a leaf's view minimum; along a branch's axis the sum of
    // its children's, across it the largest.
    QSizeF minimumSize(const MinimumSize &minimum) const;
    bool fits(const QSizeF &size, const MinimumSize &minimum) const;
    // Each area's rectangle in `rect`. Borders fall on whole pixels.
    QList<Placed> layout(const QRectF &rect, const MinimumSize &minimum, bool rightToLeft) const;
    // A branch's children's rectangles in `rect`, in child order.
    static QList<QRectF> childRects(const Node &branch, const QRectF &rect, const MinimumSize &minimum, bool rightToLeft);
    static QSizeF minimumSizeOf(const Node &node, const MinimumSize &minimum);
    // `length` shared out by `shares` (summing to 1), no part under its minimum while the minimums fit; when
    // they do not, in proportion to the minimums. Each part ends on a whole pixel.
    static QList<double> distribute(double length, const QList<double> &shares, const QList<double> &minimums);

    // A top-level key of the saved form this version keeps for its owner (Workspace's "queuePlace"); a null value removes it.
    QJsonValue extra(const QString &key) const { return m_extra.value(key); }
    void setExtra(const QString &key, const QJsonValue &value);

    QJsonObject toJson() const;
    QByteArray save() const; // toJson() in Fugue's file form (engine/json.h)
    // A tree from its saved form; the default tree with *problem set when it does not validate.
    static AreaTree fromJson(const QJsonObject &json, QString *problem = nullptr);
    static AreaTree load(const QByteArray &bytes, QString *problem = nullptr);
    // One node of the saved form (a "leaf" or "branch" object). False with *problem set when it does not
    // validate. Normalized as a loaded tree is, unless `normalized` is false: QML that draws a tree
    // (AreaNode.qml) lays out the children it was given, in their order.
    static bool nodeFromJson(const QJsonObject &json, Node *node, QString *problem = nullptr, bool normalized = true);
    static QJsonObject nodeToJson(const Node &node);

private:
    QJsonObject m_extra; // top-level keys this version does not know
    Node m_root;
};

} // namespace fugue
