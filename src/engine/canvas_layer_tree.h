#pragma once

// The layers of a canvas as an outline (#278): a tree of groups shown as one list, as Photoshop's Layers panel, Krita's layer docker and
// Blender's outliner show nested groups. The window knows no tree logic; this class does, and is tested without one.
//
//  - Rows. Every layer of the tree, groups included, is a row with an `id`: a top-level layer's id is its position (bottom first, as
//    CanvasViewModel and `fugue canvas layer` name layers), a layer inside a group takes the ids after those, in the order of a walk
//    from the top down that visits a group before its children. An id stays the same whether the groups are open or not.
//    `rows()` is what the list shows, top first: a row is listed when every group above it is open.
//  - Open state. A group opens and closes (`setOpen`, `toggle`); every group starts closed, so a canvas without nesting reads as before.
//    It is kept by the names from the top to the group (not by position), so deleting or moving a layer below it keeps it as it was;
//    `renameOpen` carries it over a rename. A closed group shows the layers it holds as its `children` count.
//  - Inheritance, as an outliner draws it: a hidden group hides what is inside it (`shown` is false for the children, while their own
//    eye keeps its own state and comes back with the group), and a locked group protects what is inside it (`lockedAbove`).
//  - Selection. `fitSelection` takes the layers hidden by a group that closed to the group itself, as Photoshop does; `levelOf` says
//    whether layers sit in one group, which is what an edit needs (CanvasViewModel edits one level at a time, LayerEdit::parent).
//  - Drop. `dropAt` turns the line the pointer is at (the gap above row `gap` of `rows()`) into where the dragged layers end among
//    the layers of their own group, or says why that line is not one: a layer moves within its group (moving it into or out of a group
//    is not offered yet).

#include <QHash>
#include <QSet>
#include <QString>

#include <vector>

#include "engine/canvas_store.h"

namespace fugue::canvas {

// A layer's place: the index of a group at the top level, then of a group in its children, ... then the layer's own index (bottom first).
using LayerPath = std::vector<int>;

struct OutlineRow {
    int id = -1;
    LayerPath path;
    int depth = 0; // groups above it
    QString name;
    bool group = false;
    int children = 0;       // layers a group holds (its own level only)
    bool open = false;      // a group whose children are listed
    bool shown = true;      // its own eye and every group's above it
    bool lockedAbove = false; // a locked group above it
};

struct DropPlace {
    bool ok = false;
    LayerPath parent; // the group the layers stay in (empty: the top level)
    int to = 0;       // LayerEdit::Move's `to`: where the lowest dragged layer ends among the others of that level
    QString why;      // when not ok: what a person can do instead
};

class LayerOutline {
public:
    // The tree's layers as rows. The open state is kept, also for a group that is not there now (an undo that brings it back finds it open).
    void setTree(const Tree &tree);

    // The rows the list shows, top first.
    const std::vector<OutlineRow> &rows() const { return m_visible; }
    // Every row whether its group is open or not, top first with a group before its children (ids are numbered in this order).
    const std::vector<OutlineRow> &allRows() const { return m_all; }
    int topCount() const { return m_top; }
    const OutlineRow *row(int id) const;
    LayerPath pathOf(int id) const; // empty for an id that is not there
    int idOf(const LayerPath &path) const; // -1 when there is none
    // How many layers the group at `parent` holds (the top level for an empty path); 0 for a path that is no group.
    int levelSize(const LayerPath &parent) const;

    bool isOpen(const LayerPath &group) const;
    // Opens or closes a group (false when the path is no group with children).
    bool setOpen(const LayerPath &group, bool open);
    bool toggle(const LayerPath &group);
    // Every group with children, open or closed.
    void setAllOpen(bool open);
    // The group at `group` (already holding its new name in the tree) was called `oldName`: its open state and that of the groups
    // inside it move to the new name.
    void renameOpen(const LayerPath &group, const QString &oldName);

    // The ids with the layers a closed group hides replaced by that group (the nearest listed one above), each once, order kept.
    std::vector<int> fitSelection(const std::vector<int> &ids) const;
    // Whether the ids are layers of one group: then `parent` is it and `indices` the layers' places in it, in the order given.
    bool levelOf(const std::vector<int> &ids, LayerPath *parent, std::vector<int> *indices) const;
    // The ids of the layers in `rows()` between two listed ids (the range of a Shift+click), top first order by row.
    std::vector<int> rangeBetween(int a, int b) const;
    DropPlace dropAt(const std::vector<int> &ids, int gap) const;

private:
    // The open flags of every row from `m_open`, then the rows the list shows.
    void listVisible();

    std::vector<OutlineRow> m_all, m_visible;
    QHash<int, int> m_byId;        // id -> index in m_all
    QHash<QString, int> m_byPath;  // "1/0/2" of a path -> id
    std::vector<QString> m_keys;   // by index in m_all: the names from the top to the layer
    QSet<QString> m_open;          // keys of open groups
    int m_top = 0;
};

} // namespace fugue::canvas
