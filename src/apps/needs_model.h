#pragma once

// The Needs panel's store (#469, epic #397, spec 12.17): what the shown app still needs, as a quiet list in the App view. It is not
// a new check: the rows are the Get ready sheet's own plan (engine/readiness.h, #379), asked again whenever the workflow, its
// models, its packs or a run's outcome change, and shown as items (a missing pack to install, a missing model with its size and
// licence). Nothing is installed or downloaded here: "Download" for one model and "Download all" ask the window to do it through
// the paths it already has (the model download line; the Get ready sheet, whose one yes covers packs, trust, environments and
// models), and "Show in graph" asks the window to switch to the graph and centre the node.
//
// The panel is there while `items` is not empty and clears by itself when a refresh finds the app ready. The plan comes from an
// injected source (the window gives the shown tab's; a test gives a fake plan), the node of an item from an injected locator.
//
// Items: [{kind ("pack", "model" or "problem"), key, name, detail (the row's quiet line), size (text, empty when none), licence,
// licenceUrl, canDownload (a model that can be had on its own: the pack rows and the models of a pack's type come with the
// sheet), action ("Download" or "Install"), node (the graph's node, empty when unknown)}].

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

#include "engine/document.h"
#include "engine/readiness.h"

namespace fugue {

class NeedsModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made by AppView")
    // Nothing is missing: the panel is not shown.
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    // "This app needs 1 pack and 2 models (7.1 GB)", for the panel's header line.
    Q_PROPERTY(QString headline READ headline NOTIFY changed)
    // Every model of the list can be downloaded on its own and no pack is among the rows: Download all starts at once (nothing to
    // approve); else it opens the Get ready sheet for the one yes.
    Q_PROPERTY(bool startsAtOnce READ startsAtOnce NOTIFY changed)
    // The window can jump to a node of the graph (builders; the app's own window has no graph).
    Q_PROPERTY(bool canShowInGraph READ canShowInGraph NOTIFY changed)

public:
    using Plan = std::function<ReadinessPlan()>;
    // The node of the shown workflow an item belongs to: the item as listed (`kind`, `key`, `name`, `hash`), the node's id or empty.
    using Locate = std::function<QString(const QVariantMap &item)>;

    explicit NeedsModel(QObject *parent = nullptr);

    void setPlanSource(Plan source) { m_source = std::move(source); }
    void setLocator(Locate locate) { m_locate = std::move(locate); m_graph = bool(m_locate); }
    // The graph is not there to jump to (the app's own window).
    void setGraphAvailable(bool available);

    bool ready() const { return m_items.isEmpty(); }
    QVariantList items() const { return m_items; }
    QString headline() const { return m_headline; }
    bool startsAtOnce() const { return m_atOnce; }
    bool canShowInGraph() const { return m_graph; }

    // The node of `document` an item belongs to: a model's, the first whose inputs hold its hash; a pack's, the first of its node types
    // (the type id's namespace is the pack). Empty when none (a node of a group is not found: the group's node is not the model's).
    static QString nodeFor(const Document &document, const QVariantMap &item);
    // Asks the source for the plan again and lists it. Cheap when the source says ready.
    Q_INVOKABLE void refresh();
    // The same, once, a moment later: several changes at the same time (a tab switched, a notice, a run) ask for one look.
    void refreshSoon();
    // Lists this plan (what refresh() does with the source's).
    void setPlan(const ReadinessPlan &plan);
    // Download the one model of row `index` (a model with a source that does not wait for a pack); a pack row, or a model that
    // does, opens the Get ready sheet as Download all does.
    Q_INVOKABLE void download(int index);
    // The whole plan once, through the Get ready path: `getReadyRequested(startNow)`.
    Q_INVOKABLE void downloadAll();
    // Switch to the graph and centre the node of row `index`: `showNodeRequested(node)`.
    Q_INVOKABLE void showInGraph(int index);

signals:
    void changed();
    // Open the Get ready sheet; with `startNow` the person's yes is already given by the click (only models, nothing to approve).
    void getReadyRequested(bool startNow);
    // Download this model (Document::models()'s form) on the window's download line.
    void downloadRequested(const QJsonObject &model);
    void showNodeRequested(const QString &node);

private:
    Plan m_source;
    Locate m_locate;
    bool m_graph = false;
    QVariantList m_items;
    QList<QJsonObject> m_entries; // each model row's workflow handle, by item index (empty object for the other rows)
    QString m_headline;
    bool m_atOnce = false;
    QTimer m_soon;
};

} // namespace fugue
