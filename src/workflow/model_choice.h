#pragma once

// Model choice (spec 2.5, "Use what you have" / "Use my file"): everything about an ask to use files that are already on disk in place
// of missing models. One module, so the rules live once and not as comments at the call sites:
//   - the ask, with its destination (a tab, and the graph or the App view) fixed when it is made;
//   - one read of the files' hashes per target, shared by the asks; cancel by the sheet's request token;
//   - one automatic swap per model; the tried marks (per tab and view) and the idle marks of the setting's own path;
//   - the result per ask, the banner, and the Undo record; the "Reading your file..." line;
//   - the open asked for last (a template whose files are still read opens only if it is still the last open asked for).
// Callers (Get ready, the Needs panel, the node card, open-with-files, the setting) reach it through Workflows, which owns one.
//
// Two adapters sit at one seam, ChoiceDestination: GraphDestination (the shown tab's document and its undo stack, below) and the App
// view's (apps/app_view_destination.h). What the module needs from its owner is ChoiceHost: one snapshot of facts and a few outputs.

#include "engine/document.h"
#include "engine/model_use.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class CanvasModel; // (the canvas model lives in the global namespace)

namespace fugue {

enum class ChoiceView { Graph, App };

// What Undo of the banner puts back: the tab, the file handle each input was changed to, and the stamp that tells the document is
// still as the choice left it (the graph's undo index).
struct ChoiceRecord {
    struct Change {
        QVariantList path; // where in the document: object keys and array indexes
        QJsonValue original, applied;
    };
    QString tab;
    int stamp = 0;
    QList<Change> changes;
    bool isEmpty() const { return tab.isEmpty(); }
};

// Where a resolved choice lands. Two implementations, so this is a real seam; tests use a fake.
class ChoiceDestination {
public:
    virtual ~ChoiceDestination() = default;
    virtual bool shown(const QString &tab) const = 0;   // this view is what the person looks at for `tab`
    virtual Uses inForce(const QString &tab) const = 0; // what it holds for `tab` and judges the tab by (the graph: nothing, the document has it)
    enum class Take { Applied, Unchanged, Refused };
    // Puts the files in place for `tab`. Unchanged: an automatic swap found those inputs changed already (nothing to change, nothing to say).
    // Refused: `tab` cannot take it now (the graph's tab is not the shown one). `record` gets the Undo record when the view has the banner's.
    virtual Take take(const QString &tab, const QList<OnDiskUse> &chosen, const Uses &resolved, bool announce, ChoiceRecord *record) = 0;
    // The record of what changed from `before` to the tab's document now (a template opened with files); empty for a view with no banner Undo.
    virtual ChoiceRecord took(const QString &tab, const Document *before) const = 0;
    virtual bool holds(const ChoiceRecord &record) const = 0; // the record's Undo still applies
    virtual bool revert(const ChoiceRecord &record) = 0;      // the banner's Undo; false when nothing was put back
};

// The shown tab's document: one undo step per choice, written to a file only when the person saves.
class GraphDestination : public ChoiceDestination {
    Q_DECLARE_TR_FUNCTIONS(GraphDestination)
public:
    GraphDestination(CanvasModel &canvas, std::function<QString()> shownTab);
    bool shown(const QString &tab) const override;
    Uses inForce(const QString &) const override { return {}; }
    Take take(const QString &tab, const QList<OnDiskUse> &chosen, const Uses &resolved, bool announce, ChoiceRecord *record) override;
    ChoiceRecord took(const QString &tab, const Document *before) const override;
    bool holds(const ChoiceRecord &record) const override;
    bool revert(const ChoiceRecord &record) override;

private:
    CanvasModel &m_canvas;
    std::function<QString()> m_shownTab;
};

// The banner of a swap (spec 2.5, "Use my file"; docs/ui-ergonomics.md 2026-11-27): a short title, one line per file put in place of a
// template's or workflow's model, and the whole text. `text` is what Workflows::notice holds and what the banner's copy button copies:
// one sentence per swap, nothing left out. Each line is {mine, theirs, minePath, tier}: the person's file, the model it stands in for,
// where the person's file is, and the quiet chip ("same model · Q8_0 · another source").
struct SwapNotice {
    QString title;
    QVariantList lines;
    QString text;
    bool isEmpty() const { return lines.isEmpty(); }
};
// Opened: a template opened with the person's files. Used: a tab already open took them (Get ready, the Needs panel, the setting).
enum class SwapKind { Opened, Used };
SwapNotice swapNotice(SwapKind kind, const QList<OnDiskUse> &applied);

// What the module reads of its owner, taken at one moment (a fact is read again where an earlier step may have changed it).
struct ChoiceFacts {
    QString shownTab;       // empty: no tab
    QStringList openTabs;
    bool sheetOpen = false; // a Get ready the person is deciding in
    bool sheetReview = false; // ... and it is on its review page
};

// The owner's side: the facts and the outputs. The three reads that cost something stay calls: the setting (a file is read), the key of
// the shown tab's models and the covered models (a folder walk); the idle check needs the key before the models are walked.
struct ChoiceHost {
    std::function<ChoiceFacts()> facts;
    std::function<bool()> autoOn; // the setting says "use" (and there is a registry)
    std::function<QByteArray()> modelsKey;
    std::function<QList<OnDiskUse>()> covered; // the shown tab's missing models that have a file of the same weights
    // Reads `path`'s hash on the worker: progress(percent) as it goes, done(hash) at the end ("" when unreadable).
    std::function<void(const QString &path, std::function<void(int)> progress, std::function<void(const QString &)> done)> hash;
    std::function<void(const QString &)> say;          // the banner
    std::function<void(const SwapNotice &)> sayChoice; // the banner of a swap, in lines (unset: `say` gets its text)
    std::function<QString()> saying;                   // the banner's text now
    std::function<void(const QString &)> sayInSheet;   // the Get ready sheet's own line
    std::function<void(bool force)> changed;           // the models a tab has changed: forget the on-disk facts, read the banner's again
    std::function<void()> endReading;                  // the banner's reading line is gone: back to the line it covered
    std::function<void()> offerSheet;                  // what waited for the files (Get ready of a tab just opened) opens now
    std::function<void(std::function<void()>)> later;  // runs after a modal Get ready sheet closes (at once when none is open)
};

class ModelChoice : public QObject {
    Q_OBJECT
public:
    // Who asked: a Person (a sheet, the Needs panel, the node card), or the setting; SettingOnOpen is the setting on a tab just opened,
    // whose end may open Get ready.
    enum class Source { Person, Setting, SettingOnOpen };
    enum class Check { TabShown, ModelsChanged, Offer };

    ModelChoice(ChoiceHost host, ChoiceDestination &graph, QObject *parent = nullptr);
    void setApp(ChoiceDestination *app) { m_app = app; } // null: no App view

    // The ask: `hashes` (empty: all that are covered) of the shown tab, for the view the person is looking at now. `token` is a sheet's
    // request id, echoed by `finished`; 0 for any other.
    void ask(const QStringList &hashes, int token);
    void cancel(int token); // that request only: another surface's read of the same files goes on
    // The setting's own path (a tab shown, models that may have changed, a tab just opened): true when a read started.
    bool check(Check kind);
    bool offerAfterRead();  // an automatic read of the shown tab is going: its end opens Get ready; true then
    void modelsMayHaveChanged() { m_idle.clear(); }
    void tabClosed(const QString &tab);
    // The open asked for last ("open:<template>" or "path:<file>"); only Workflows::beginOpen calls it.
    void openAsked(const QString &key);
    // Opens a template with the person's files once they are read, unless another open was asked for meanwhile. `openIt` opens it with the
    // resolved handles; `original` is the template's own file (what Undo goes back to); `opened` runs when the tab exists, or with false.
    bool openWith(const QString &key, const QList<OnDiskUse> &uses, const std::function<bool(const Uses &)> &openIt, const QString &original,
                  std::function<void(bool)> opened);
    // The person chose not to use these (a template opened for download): neither view tries them.
    void declined(const QString &tab, const QStringList &hashes);
    struct Opening { // while a template opens, the setting's path leaves it alone
        explicit Opening(ModelChoice &choice) : m_choice(choice) { m_choice.m_opening = true; }
        ~Opening() { m_choice.m_opening = false; }
        ModelChoice &m_choice;
    };

    QVariantMap reading() const { return m_reading; } // hash -> percent
    bool hashing() const { return !m_hashing.isEmpty(); }
    bool hasRecord() const { return !m_record.isEmpty(); }
    bool undoable() const; // the banner's Undo: its tab is shown, unedited, and the banner still says something
    void undo();
    Uses inForce(const QString &tab) const { return m_app ? m_app->inForce(tab) : Uses(); }

signals:
    void applied();
    void finished(const QStringList &applied, const QStringList &failed, int token); // the hashes now using their file, and those whose file could not be read
    void readingChanged();
    void hashingChanged();
    void recordChanged(); // the banner's Undo was set

private:
    struct Ask {
        int token = 0;
        QString tab;
        ChoiceView view = ChoiceView::Graph;
        QList<OnDiskUse> chosen;
        bool automatic = false;
        bool offer = false;
    };
    struct Job {
        QStringList wanted;
        QList<Ask> asks;
        bool cancelled = false;
    };
    ChoiceDestination *dest(ChoiceView view);
    void start(const QStringList &hashes, int token, Source source);
    bool apply(const QString &tab, const QList<OnDiskUse> &chosen, const Uses &resolved, ChoiceView view, bool announce);
    void announce(const QString &tab, ChoiceView view, const QList<OnDiskUse> &chosen, const Uses &resolved, const ChoiceRecord &record);
    void sayChoice(const SwapNotice &notice);
    bool finishOpen(const QList<OnDiskUse> &uses, const Uses &resolved, const QStringList &failed, const std::function<bool(const Uses &)> &openIt,
                    const QString &original);
    void hashUses(const QList<OnDiskUse> &uses, std::function<void(const Uses &, const QStringList &)> done,
                  std::function<void(const OnDiskUse &, int)> progress);
    void markTried(const QString &tab, ChoiceView view, const QString &hash);
    void markBoth(const QString &tab, const QString &hash);
    static QString triedKey(const QString &tab, ChoiceView view, const QString &hash);
    bool autoPending() const;
    void setHashing(const QString &key, bool on);
    void setReading(const QStringList &hashes, int percent);
    void clearReading(const QStringList &hashes);
    void sayReading(int percent);
    void endReadingLine();
    static QString hashOf(const OnDiskUse &use);

    ChoiceHost m_host;
    ChoiceDestination &m_graph;
    ChoiceDestination *m_app = nullptr;
    QHash<QString, Job> m_jobs;       // by target ("use:<tab> <hashes>")
    QSet<QString> m_hashing;          // the targets whose files are being read ("use:...", "open:<template>")
    QVariantMap m_reading;
    QString m_readLine;               // the exact banner text this module said for a read, so only that is taken back
    QHash<QString, QByteArray> m_idle; // tab -> its models when the setting's path found nothing to do
    QSet<QString> m_tried;            // "<tab> <a|g> <hash>" of every model the setting's path asked for
    QString m_latestOpen;
    bool m_opening = false;
    ChoiceRecord m_record;
};

} // namespace fugue
