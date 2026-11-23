#pragma once

// Runs from the canvas (spec 12.4, 12.7, 10.3): the window's runs, App.runs in QML.
//
// It starts runs of the workflow on the canvas through RunService (run/run_service.h) and
// shows what they do on the nodes, through CanvasModel::setRunState and setRunProgress:
//   - the state of 10.3 on every node: stale, cached, running, frozen, error, skipped. Before
//     and between runs it comes from RunService::check(), made 300 ms after the last edit and
//     when a workflow is shown; during a run from its events. A node's error stays until the
//     workflow is edited or run again;
//   - per node: progress (at most one canvas update per node per 33 ms), the time taken, the
//     last value on each output port (a number, a short text, a thumbnail of the last image),
//     and warnings;
//   - nodes inside a group (path ids "g7/first") show on the group's node on the canvas, with
//     the full path id in the error, and in `errors`, which lists every error of the last run
//     with its path id.
// What a tab's runs did is kept per tab and shown again when the tab comes back; a run
// started in one tab never paints another.
//
// Commands: run.start (Ctrl+Enter), run.start-first (Ctrl+Shift+Enter, ahead of the queue),
// run.to-here (Shift+Enter: up to the one selected node, adding no preview), run.cancel
// (Ctrl+Alt+Enter, and Escape while a run is going: the `run` context is on while one of the
// window's runs is waiting or active). Cancel takes the active run, else the last waiting.
// Each start takes the document as it is and then advances the seeds set to increment
// (WidgetEdit::advanceSeeds), so the run uses the number shown and the next run the next one.
// `batch` is how many runs one start makes (ComfyUI's batch count, #60): each takes the
// document as it is then, so a seed set to increment gives each its own number; they queue
// together, in order, also ahead of the queue with run.start-first. start() returns the first id.

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

#include "run/run_service.h"

class CanvasModel;

namespace fugue {

class CommandRegistry;
class Workflows;

class CanvasRuns : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // The run pill (docs/canvas-look.md 11): any run of the window waiting or active.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)      // one is active
    Q_PROPERTY(int count READ count NOTIFY changed)           // waiting and active
    Q_PROPERTY(QString activeNode READ activeNode NOTIFY progressChanged) // its running node's title
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)       // that node's, or -1
    // The nodes of the shown workflow that are running now (their incoming wires show dots).
    Q_PROPERTY(QStringList runningNodes READ runningNodes NOTIFY runningNodesChanged)
    // Their incoming wires, for the running dots (docs/canvas-look.md 8.2): {path, color}.
    Q_PROPERTY(QVariantList runningWires READ runningWires NOTIFY runningNodesChanged)
    // The errors of the shown workflow's last run: {node (path id), shown (canvas node id),
    // message, code, hint}.
    Q_PROPERTY(QVariantList errors READ errors NOTIFY errorsChanged)
    // How the last run of the shown workflow ended: "", "ok", "failed" or "cancelled".
    Q_PROPERTY(QString lastStatus READ lastStatus NOTIFY errorsChanged)
    // True while something that is not the canvas holds the keys the `run` context owns (the queue
    // panel has the focus, and Escape closes it): run.cancel's keys sleep until it is false again.
    Q_PROPERTY(bool yieldKeys READ yieldKeys WRITE setYieldKeys NOTIFY yieldKeysChanged)
    // How many runs one start makes, 1 to maxBatch (#60).
    Q_PROPERTY(int batch READ batch WRITE setBatch NOTIFY batchChanged)

public:
    // `registry` tells which inputs are image files (a Load Image's) for their thumbnails.
    CanvasRuns(CanvasModel *canvas, Workflows *workflows, RunService *service, const Registry *registry = nullptr,
               QObject *parent = nullptr);
    ~CanvasRuns() override;

    void registerCommands(CommandRegistry &commands);

    bool busy() const { return !m_order.isEmpty(); }
    bool running() const { return !m_active.isEmpty(); }
    int count() const { return int(m_order.size()); }
    QString activeNode() const { return m_activeNode; }
    qreal progress() const { return m_activeProgress; }
    QStringList runningNodes() const;
    QVariantList runningWires() const;
    QVariantList errors() const;
    QString lastStatus() const;
    static constexpr int maxBatch = 99;
    bool yieldKeys() const { return m_yieldKeys; }
    void setYieldKeys(bool yield);
    int batch() const { return m_batch; }
    void setBatch(int batch);

    // The run ids started here, waiting or active, in queue order.
    QStringList runs() const { return m_order; }

    Q_INVOKABLE QString start(bool first = false);
    Q_INVOKABLE QString runToSelected();
    Q_INVOKABLE void cancel();
    // Shows the results of a run that ended on the nodes of the shown workflow, as they were when it
    // ended (the queue panel's history): `workflow` is the file the run started from. False,
    // and nothing shown, when that is not the file of the shown tab (a run's node ids mean
    // something only in its own workflow) or the tab is not a saved one.
    Q_INVOKABLE bool showResults(const QString &run, const QString &workflow);
    // A check now rather than after the edit pause (tests).
    Q_INVOKABLE void checkNow();
    // The registry was filled again, or trust given (#70): the run thread reloads too, and the
    // nodes are checked again against the new types (a placeholder that turned real can run).
    void refreshNodeTypes();
    // The full image file of the node's picture slot (its last result, else the file its input
    // names), for Open, Copy and Save Image; empty when it shows none or the file is gone.
    // Never the thumbnail, which is a 512 px copy.
    Q_INVOKABLE QString imageFile(const QString &node) const;
    // The same as a file URL, for QML (empty without one).
    Q_INVOKABLE QUrl imageUrl(const QString &node) const;
    // The picture arriving at an input of a node (the paint window's, #181): {file, width, height} of the
    // full file of what the linked node shows (its result, else its file's image); empty when the input
    // is not linked, the node has not run, or the file is gone.
    Q_INVOKABLE QVariantMap inputPicture(const QString &node, const QString &input) const;
    // The file an input-file literal names (a painted mask, `inputs/x.png`), as the run reads it: a
    // relative path is the workflow folder's. Empty when the literal is empty or the file is not there.
    Q_INVOKABLE QString inputFile(const QString &literal) const;
    // The full file of one picture of a Compare Images node ("a", "b", "difference" or
    // "composite"), empty when there is none or the file is gone.
    Q_INVOKABLE QString compareFile(const QString &node, const QString &which) const;
    // The picture the shown workflow last made, for a shared package's preview (#91): the newest
    // result a Save Image or Preview shows; empty when none shows one.
    Q_INVOKABLE QString lastPicture() const;
    // Selects the canvas node an error is shown on and puts it in view (the error list).
    Q_INVOKABLE void showError(const QString &node);

signals:
    void changed();
    void batchChanged();
    void yieldKeysChanged();
    void progressChanged();
    void runningNodesChanged();
    void errorsChanged();
    void finished(const QString &run, const QString &status);
    void checkApplied();
    void inputPreviewed(const QString &node);

private:
    struct NodeView {
        QString state;
        qreal progress = -1;
        qreal seconds = -1;
        QString error;
        QString errorDetail; // the whole text when `error` is the short form (a missing pack's words); else empty
        QVariantMap pack;    // that missing pack: {id, name, text, installable}; else empty (#264)
        QStringList warnings;
        QString notice; // a check's word on why the node cannot run now, and the fix (8.5)
        QString thumbnail;
        int thumbnailWidth = 0;
        int thumbnailHeight = 0;
        QVariantMap values;
        QString thumbnailFile; // the file a sink wrote, named under its picture
        QString imagePath;     // the full image behind `thumbnail`, for Open, Copy and Save Image
        QVariantMap media;     // a video or sound result's player: type, url, label, width, height, duration, file
        QVariantMap compare;   // Compare Images: a, b, difference, composite ({url, source, width, height})
        bool live = false;     // `thumbnail` is a preview of the run going on now (9.4)
        QString text;          // a text, number or media label shown in the slot
        qreal textAspect = 0.5;
        QString inputThumbnail; // the image of the node's input file, before it runs
        int inputWidth = 0;
        int inputHeight = 0;
        bool inputMissing = false;
        QHash<QString, QString> inner; // a group's nodes: path id -> state
        QHash<QString, qreal> innerSeconds;
        void clearError() { error.clear(); errorDetail.clear(); pack.clear(); }
    };
    struct TabRuns {
        QHash<QString, NodeView> nodes; // by path id: every node, and each group instance holding one
        QVariantList errors;
        QString lastStatus;
        int busy = 0;   // its runs waiting or active
        int epoch = 0;  // runs started, so an older check does not undo a run's states
    };

    // Path ids and the level shown: a path's card (empty when the level has none for it), and
    // the path of a card.
    QString rowFor(const QString &key) const;
    QString keyFor(const QString &node) const;
    static qreal progressOf(const NodeView &view);
    QString startRun(const QString &target, bool first);
    QString startOne(const QString &tab, const QString &target, bool first);
    QString currentTab() const;
    void onEvent(const QString &run, const QJsonObject &event);
    void onChecked(int ticket, const QJsonObject &nodes, const QJsonArray &problems);
    void scheduleCheck(bool edit);
    void showTab();
    void show(const QString &tab, const QString &node);
    // The picture the node shows when it crops: what its image input receives, from the node linked
    // to it (its last result, else its input file's image). Empty `thumbnail` without one.
    struct Picture {
        QString thumbnail, path;
        int width = 0, height = 0;
    };
    Picture cropSource(const QString &tab, const QString &node) const;
    Picture pictureOf(const QString &tab, const QString &from) const;
    void flushProgress();
    void setContext();
    void readNodes();
    void refreshInputs();
    void requestInputPreview(const QString &node);
    void onFilePreviewed(const QString &token, const QString &path, const QJsonObject &thumbnail);
    void updateRunning();
    QString nodeTitle(const QString &tab, const QString &path) const;
    static QString aggregate(const NodeView &view);

    CanvasModel *m_canvas;
    // Puts `error` on `view`: its message, and for a node of a pack that is not installed the words of
    // "Needs the X pack" with the full text kept for the tooltip (#264). `path` names an inner node.
    void setError(NodeView &view, const Error &error, const QString &path, bool inner) const;
    Workflows *m_workflows;
    RunService *m_service;
    const Registry *m_registry;
    struct NodeInfo {
        bool picture = true;      // the node shows its picture now (#263): the mode, its type and its override say so
        bool showsResult = false; // Preview, Save Image: a placeholder until a result comes
        bool savesFile = false;   // Save Image, Save Video, Save Audio: its chip or player names the file it wrote
        bool compares = false;    // Compare Images: its slot is a viewer of A and B (#62)
        QString labelA, labelB;   // what produced A and B: the linked node's title
        QString imageFile;        // the image file its file input names
        bool crops = false;       // Crop: its slot shows the picture arriving at its input, under the frame (#73)
        QString paintsOver;       // a node with a mask widget over an image input (Paint Mask): that input's name
        QString cropFrom;         // the path id of the node that feeds the followed input, if one does
        // The slot shows the picture arriving at an input of the node, not its own result.
        bool followsInput() const { return crops || !paintsOver.isEmpty(); }
    };
    // The nodes of the shown level whose pictures are not made: hidden ones that no shown Crop or Paint Mask follows (#263).
    QSet<QString> noPictures() const;
    QHash<QString, NodeInfo> m_nodeInfo; // of the shown workflow
    QSet<QString> m_rowsMade;            // its nodes whose cards made their rows
    CommandRegistry *m_commands = nullptr;
    QHash<QString, TabRuns> m_tabs;
    QHash<QString, QString> m_tabOf; // run id -> tab id
    QStringList m_order;
    QSet<QString> m_first; // the waiting runs started ahead of the queue (they keep their order)
    int m_batch = 1;
    bool m_yieldKeys = false;
    QString m_active;
    QString m_activeNode;
    QString m_activeNodePath;
    qreal m_activeProgress = -1;
    QString m_shownTab;
    QTimer m_checkTimer;
    QTimer m_progressTimer;
    QHash<QString, qreal> m_pendingProgress; // canvas node id -> progress, of the shown tab
    QStringList m_running; // runningNodes() as last announced
    QHash<QString, QVariantMap> m_shown;     // what each node of the shown tab was last given
    // The latest check asked (m_ticket), the only one the run thread answers and the only
    // one applied: a newer one replaces it, taking over its edit.
    int m_ticket = 0;
    struct Ticket {
        QString tab;
        int epoch = 0;
        bool edit = false; // asked after an edit: the last run's errors go
    };
    Ticket m_asked;
    bool m_checkFromEdit = false;
};

} // namespace fugue
