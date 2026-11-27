#pragma once

// The Needs panel's store (#469, epic #397, spec 12.17): what the shown app still needs, as a quiet list in the App view. It is not
// a new check: the rows are the Get ready sheet's own plan (engine/readiness.h, #379), asked again whenever the workflow, its
// models, its packs or a run's outcome change, and shown as items (a missing pack to install, a missing model with its size and
// licence). Nothing is installed or downloaded here: "Download" for one model and "Download all" ask the window to do it through
// the paths it already has (the model download line; the Get ready sheet, whose one yes covers packs, trust, environments and
// models), and "Show in graph" asks the window to switch to the graph and centre the node.
//
// The panel is there while `items` has a row that is not optional and clears by itself when a refresh finds the app ready. The plan comes from an
// injected source (the window gives the shown tab's; a test gives a fake plan), the node of an item from an injected locator.
//
// Items: [{kind ("pack", "model", "optional" (a model only another Switch branch needs, #594: it does not make the panel show) or "problem"), key, name, detail (the row's quiet line), size (text, empty when none), licence,
// licenceUrl, canDownload (a model that can be had on its own: the pack rows and the models of a pack's type come with the
// sheet), use (a model with a file of the same weights on disk: {file, same, tier, ...}, else empty), action ("Download" or "Install"), node (the graph's node, empty when unknown)}].

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <algorithm>
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
    // Use what you have (#605, UI design 2026-11-26): how many models of the list have a file of the same weights on disk, and what the
    // header chip does with them: "use" (every model is covered and no pack: the files go in at once), "getReady" (some are covered:
    // the sheet asks, with Use my file chosen for them), "downloadAll" (none; starts at once when it can) or "getReady" for the rest.
    Q_PROPERTY(int coveredCount READ coveredCount NOTIFY changed)
    Q_PROPERTY(QString headerAction READ headerAction NOTIFY changed)
    // The files being read, per model hash: percent (-1 unknown). A row says "Reading your file…" and takes no second press.
    Q_PROPERTY(QVariantMap reading READ reading NOTIFY readingChanged)
    // Whether one of this list's own models is being read (#626): the header chip waits for those only, not for a file another tab reads.
    Q_PROPERTY(bool readingOwn READ readingOwn NOTIFY readingOwnChanged)
    // The window can jump to a node of the graph (builders; the app's own window has no graph).
    Q_PROPERTY(bool canShowInGraph READ canShowInGraph NOTIFY changed)

public:
    using Plan = std::function<ReadinessPlan()>;
    // The node of the shown workflow an item belongs to: the item as listed (`kind`, `key`, `name`, `hash`), the node's id or empty.
    using Locate = std::function<QString(const QVariantMap &item)>;

    explicit NeedsModel(QObject *parent = nullptr);

    void setPlanSource(Plan source) { m_source = std::move(source); }
    // Use what you have (#605): for a missing model's hash, the file on disk of the same weights as {file, same, tier, ...}, or empty. A
    // model row with one carries it as `use`; choosing it (useOnDisk) leaves the panel's list, because the window's plan leaves the model out.
    using UseSource = std::function<QVariantMap(const QString &hash)>;
    void setUseSource(UseSource source) { m_useSource = std::move(source); }
    void setLocator(Locate locate) { m_locate = std::move(locate); m_graph = bool(m_locate); }
    // The graph is not there to jump to (the app's own window).
    void setGraphAvailable(bool available);

    bool ready() const {
        return std::none_of(m_items.cbegin(), m_items.cend(), [](const QVariant &item) { return item.toMap().value(QStringLiteral("kind")).toString() != QLatin1String("optional"); });
    }
    QVariantList items() const { return m_items; }
    QString headline() const { return m_headline; }
    bool startsAtOnce() const { return m_atOnce; }
    bool canShowInGraph() const { return m_graph; }
    int coveredCount() const { return m_covered; }
    QString headerAction() const { return m_action; }
    QVariantMap reading() const { return m_reading; }
    void setReading(const QVariantMap &reading);
    bool readingOwn() const { return m_readingOwn; }

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
    // Use the file on disk in place of the model of row `index` (a row with `use`): `useRequested({hash})`.
    Q_INVOKABLE void useOnDisk(int index);
    // The header chip: `useRequested` with every hash when all are covered, else the whole plan once through the Get ready path:
    // `getReadyRequested(startNow)` (the sheet leaves the covered models on Use my file).
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
    // The person chose to use the files they have for the models with these hashes (#605): one row's Use my file, or the header's when
    // every model is covered.
    void useRequested(const QStringList &hashes);
    void readingChanged();
    void readingOwnChanged();

private:
    Plan m_source;
    Locate m_locate;
    UseSource m_useSource;
    bool m_graph = false;
    QVariantList m_items;
    QList<QJsonObject> m_entries; // each model row's workflow handle, by item index (empty object for the other rows)
    QString m_headline;
    bool m_atOnce = false;
    int m_covered = 0;
    QString m_action = QStringLiteral("downloadAll");
    QVariantMap m_reading;
    bool m_readingOwn = false;
    void updateReadingOwn();
    QTimer m_soon;
};

} // namespace fugue
