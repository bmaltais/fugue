#pragma once

// The view types an area can show (#217, spec 12.16): one row per type, as the side panels' table is
// (panels/side_panels.cpp), so a new view is one row. A row says the type's id (what the area tree saves),
// its name (translated), its glyph, the QML component that draws it, its minimum size and how many areas
// may show it at once.
//
// Names: the node graph is **Graph** (`graph`) and the picture editor of #216 will be **Canvas** (`canvas`).
// The code's older names keep "canvas" for the node graph (FugueCanvas, App.canvasModel); a new name never
// does.
//
// Multiplicity: `Many` areas may show the type; `OnePerTarget`: one area per node or canvas it edits (the Mask
// editor, the Canvas editor); `OnePerWindow`: one area in the window. The Graph is one per window until a
// second Graph has its own pointer, link drag and zoom requests (docs/workspaces-research.md 5.5, risk 2).
//
// A type no row registers (a newer build's, kept in a saved tree) is drawn as a placeholder
// (workspace/AreaPlaceholder.qml) of kPlaceholderMinimum.

#include <QList>
#include <QObject>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class ViewRegistry : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++")

public:
    enum class Multiplicity { Many, OnePerTarget, OnePerWindow };
    struct ViewType {
        QString id;
        const char *name = ""; // English; translated in the "View" context
        QString glyph;         // Lucide name (share/fonts/lucide-fugue.txt)
        QUrl component;
        QSizeF minimum;        // pixels
        Multiplicity multiplicity = Multiplicity::Many;
    };
    static const QString kGraph;
    static constexpr QSizeF kPlaceholderMinimum{160, 120};

    // The built-in types (today the Graph alone).
    explicit ViewRegistry(QObject *parent = nullptr);

    // Adds a type, or replaces the one of its id (a test's own views).
    void add(const ViewType &type);

    Q_INVOKABLE QStringList ids() const;
    Q_INVOKABLE bool contains(const QString &id) const { return find(id) != nullptr; }
    Q_INVOKABLE QString name(const QString &id) const; // translated; the id itself for an unknown type
    Q_INVOKABLE QString glyph(const QString &id) const;
    Q_INVOKABLE QUrl component(const QString &id) const; // empty for an unknown type
    Q_INVOKABLE QSizeF minimumSize(const QString &id) const; // kPlaceholderMinimum for an unknown type
    // "many", "one-per-target" or "one-per-window"; "many" for an unknown type.
    Q_INVOKABLE QString multiplicity(const QString &id) const;

private:
    const ViewType *find(const QString &id) const;

    QList<ViewType> m_types;
};

} // namespace fugue
