#include "workflow/model_choice.h"

#include "canvas/canvas_model.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUndoStack>

#include <algorithm>
#include <memory>

namespace fugue {

namespace {

bool isHandle(const QJsonValue &value) { return value.isObject() && value.toObject().value(QStringLiteral("resource")).isString(); }

// What the file's handle replaced, per input: where it is in the document, the template's own value and the one it was opened with.
// Only handles (objects with a "resource") are compared: the other changes of an open (file inputs taken in) are not Undo's.
void diffHandles(const QJsonValue &before, const QJsonValue &after, const QVariantList &path, QList<ChoiceRecord::Change> &out) {
    if (before == after)
        return;
    if (isHandle(before) || isHandle(after)) {
        out.append({path, before, after});
        return;
    }
    if (before.isObject() && after.isObject()) {
        const QJsonObject a = before.toObject(), b = after.toObject();
        for (auto it = a.begin(); it != a.end(); ++it)
            diffHandles(it.value(), b.value(it.key()), path + QVariantList{it.key()}, out);
    } else if (before.isArray() && after.isArray() && before.toArray().size() == after.toArray().size()) {
        for (int i = 0; i < before.toArray().size(); ++i)
            diffHandles(before.toArray().at(i), after.toArray().at(i), path + QVariantList{i}, out);
    }
}

QJsonValue valueAt(const QJsonValue &from, const QVariantList &path, int depth = 0) {
    if (depth == path.size())
        return from;
    const QVariant &step = path.at(depth);
    return step.typeId() == QMetaType::Int ? valueAt(from.toArray().at(step.toInt()), path, depth + 1)
                                           : valueAt(from.toObject().value(step.toString()), path, depth + 1);
}

QJsonValue withValueAt(const QJsonValue &from, const QVariantList &path, const QJsonValue &value, int depth = 0) {
    if (depth == path.size())
        return value;
    const QVariant &step = path.at(depth);
    if (step.typeId() == QMetaType::Int) {
        QJsonArray array = from.toArray();
        array.replace(step.toInt(), withValueAt(array.at(step.toInt()), path, value, depth + 1));
        return array;
    }
    QJsonObject object = from.toObject();
    object.insert(step.toString(), withValueAt(object.value(step.toString()), path, value, depth + 1));
    return object;
}

} // namespace

// --- the graph's destination ---

GraphDestination::GraphDestination(CanvasModel &canvas, std::function<QString()> shownTab) : m_canvas(canvas), m_shownTab(std::move(shownTab)) {}

bool GraphDestination::shown(const QString &tab) const { return m_shownTab() == tab; }

ChoiceDestination::Take GraphDestination::take(const QString &tab, const QList<OnDiskUse> &, const Uses &resolved, bool announce, ChoiceRecord *record) {
    if (m_shownTab() != tab)
        return Take::Refused; // another tab is shown: the document on the canvas is not this one's
    // The document itself, one undo step; the file is written only when the person saves it.
    const Document before = m_canvas.document();
    const Document after = withUses(before, resolved);
    if (announce && after.toJson() == before.toJson())
        return Take::Unchanged; // the person changed those inputs while the file was read: nothing to change, nothing to say
    m_canvas.commitWhole(after, tr("Use the files you have"));
    if (record)
        *record = took(tab, &before);
    return Take::Applied;
}

ChoiceRecord GraphDestination::took(const QString &tab, const Document *before) const {
    // Undo puts back exactly the inputs that were changed, each its own value (two models that share one file get two originals).
    ChoiceRecord record{tab, m_canvas.undoStack()->index(), {}};
    if (before)
        diffHandles(before->toJson(), m_canvas.document().toJson(), {}, record.changes);
    return record;
}

bool GraphDestination::holds(const ChoiceRecord &record) const {
    // Offered for the tab it was made in until that tab is edited.
    return !record.isEmpty() && m_shownTab() == record.tab && m_canvas.undoStack()->index() == record.stamp;
}

bool GraphDestination::revert(const ChoiceRecord &record) {
    QJsonValue json = m_canvas.document().toJson();
    for (const auto &change : record.changes)
        if (valueAt(json, change.path) == change.applied) // an input edited since stays as it is
            json = withValueAt(json, change.path, change.original);
    const Result<Document> back = Document::fromJson(json.toObject());
    if (!back)
        return false;
    m_canvas.commitWhole(back.value(), tr("Back to the template's models"));
    return true;
}

// --- the module ---

ModelChoice::ModelChoice(ChoiceHost host, ChoiceDestination &graph, QObject *parent) : QObject(parent), m_host(std::move(host)), m_graph(graph) {}

QString ModelChoice::hashOf(const OnDiskUse &use) { return use.wanted.value(QStringLiteral("hash")).toString(); }

ChoiceDestination *ModelChoice::dest(ChoiceView view) { return view == ChoiceView::App ? m_app : &m_graph; }

QString ModelChoice::triedKey(const QString &tab, ChoiceView view, const QString &hash) {
    return tab + QLatin1Char(' ') + (view == ChoiceView::App ? QLatin1Char('a') : QLatin1Char('g')) + QLatin1Char(' ') + hash;
}

void ModelChoice::markTried(const QString &tab, ChoiceView view, const QString &hash) { m_tried.insert(triedKey(tab, view, hash)); }

void ModelChoice::markBoth(const QString &tab, const QString &hash) {
    markTried(tab, ChoiceView::App, hash);
    markTried(tab, ChoiceView::Graph, hash);
}

void ModelChoice::declined(const QString &tab, const QStringList &hashes) {
    for (const QString &hash : hashes)
        markBoth(tab, hash);
}

void ModelChoice::tabClosed(const QString &tab) {
    m_idle.remove(tab);
    for (auto it = m_tried.begin(); it != m_tried.end();)
        it = it->startsWith(tab + QLatin1Char(' ')) ? m_tried.erase(it) : std::next(it);
}

void ModelChoice::openAsked(const QString &key) { m_latestOpen = key; }

void ModelChoice::setReading(const QStringList &hashes, int percent) {
    for (const QString &hash : hashes)
        m_reading.insert(hash, percent);
    emit readingChanged();
}

void ModelChoice::clearReading(const QStringList &hashes) {
    bool changed = false;
    for (const QString &hash : hashes)
        changed |= m_reading.remove(hash) > 0;
    if (changed)
        emit readingChanged();
}

void ModelChoice::setHashing(const QString &key, bool on) {
    const bool was = hashing();
    if (on)
        m_hashing.insert(key);
    else
        m_hashing.remove(key);
    if (was != hashing())
        emit hashingChanged();
}

void ModelChoice::sayReading(int percent) {
    m_readLine = tr("Reading your file… %1%").arg(percent);
    m_host.say(m_readLine);
}

void ModelChoice::endReadingLine() {
    if (m_readLine.isEmpty() || m_host.saying() != m_readLine)
        return;
    m_readLine.clear();
    m_host.endReading();
}

void ModelChoice::cancel(int token) {
    // The sheet's request only: another surface's read of the same or other files goes on. (The hasher has no way to stop one job: the
    // file is still read to its end and remembered, and its result is dropped here.)
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        const auto found = std::find_if(it->asks.begin(), it->asks.end(), [token](const Ask &each) { return each.token == token; });
        if (found == it->asks.end())
            continue;
        it->asks.erase(found);
        if (it->asks.isEmpty()) {
            it->cancelled = true;
            clearReading(it->wanted);
        }
        return;
    }
}

void ModelChoice::hashUses(const QList<OnDiskUse> &uses, std::function<void(const Uses &, const QStringList &)> done,
                           std::function<void(const OnDiskUse &, int)> progress) {
    // A file's hash is read once and remembered (a file the pickers hashed before is known): on the worker, never the window's thread.
    struct Chain {
        Uses resolved;
        QStringList failed;
        std::function<void(int)> step;
    };
    const auto chain = std::make_shared<Chain>();
    // The step holds the chain weakly: the pending read holds it, so a finished chain frees itself (no cycle).
    chain->step = [this, uses, done = std::move(done), progress = std::move(progress), weak = std::weak_ptr<Chain>(chain)](int index) {
        const std::shared_ptr<Chain> self = weak.lock();
        if (!self)
            return;
        if (index >= uses.size()) {
            const Uses resolved = self->resolved;
            const QStringList failed = self->failed;
            done(resolved, failed);
            return;
        }
        const OnDiskUse use = uses.at(index);
        m_host.hash(
            use.match.path, progress ? [progress, use](int percent) { progress(use, percent); } : std::function<void(int)>(),
            [use, self, index](const QString &hash) {
                if (hash.isEmpty())
                    self->failed << use.match.name;
                else
                    self->resolved.insert(use.wanted.value(QStringLiteral("hash")).toString(), useHandle(use, hash));
                self->step(index + 1);
            });
    };
    chain->step(0);
}

void ModelChoice::ask(const QStringList &hashes, int token) { start(hashes, token, Source::Person); }

void ModelChoice::start(const QStringList &hashes, int token, Source source) {
    const bool automatic = source != Source::Person;
    const ChoiceFacts now = m_host.facts();
    if (now.shownTab.isEmpty()) {
        emit finished({}, hashes, token);
        return;
    }
    QList<OnDiskUse> chosen;
    QStringList wanted;
    for (const OnDiskUse &use : m_host.covered()) {
        const QString hash = hashOf(use);
        if (hashes.isEmpty() || hashes.contains(hash)) {
            chosen << use;
            wanted << hash;
        }
    }
    if (chosen.isEmpty()) {
        emit finished({}, hashes, token); // the file is gone or the model is here now
        return;
    }
    const QString tab = now.shownTab;
    wanted.sort();
    const QString key = QStringLiteral("use:") + tab + QLatin1Char(' ') + wanted.join(QLatin1Char(' '));
    // The file reads are shared per target, but each ask carries its own destination (the tab, and the App view or the graph: decided now, by
    // what the person is looking at) and applies it when the result arrives; the completion reads nothing else from the moment of the ask.
    const bool running = m_hashing.contains(key);
    Job &job = m_jobs[key];
    job.wanted = wanted;
    job.asks.append({token, tab, m_app && m_app->shown(tab) ? ChoiceView::App : ChoiceView::Graph, chosen, automatic, source == Source::SettingOnOpen});
    if (running) {
        if (job.cancelled) { // the read of a cancelled sheet is still going: it counts again, for this ask only
            job.cancelled = false;
            setReading(wanted, -1);
        }
        return;
    }
    job.cancelled = false;
    setHashing(key, true);
    setReading(wanted, -1); // every row of the choice says so; the one being read has its percent
    hashUses(
        chosen,
        [this, wanted, key](const Uses &resolved, const QStringList &failed) {
            setHashing(key, false);
            const Job finishedJob = m_jobs.take(key);
            if (finishedJob.cancelled || finishedJob.asks.isEmpty())
                return; // nobody wants it any more (the sheet was cancelled while it was read)
            clearReading(wanted);
            bool anyApplied = false, anyDropped = false;
            QList<QPair<int, QStringList>> done, notRead;
            // Each distinct destination (tab, view, files) takes the result once, however many asks wanted it; every ask is still answered.
            QHash<QString, bool> appliedTo;
            bool offerNow = false;
            for (const Ask &each : finishedJob.asks) {
                QStringList chosenHashes;
                for (const OnDiskUse &use : each.chosen)
                    chosenHashes << hashOf(use);
                if (each.automatic) {
                    // Not a failure: the tab is not the one shown (a session restored several), or the person opened Get ready meanwhile and
                    // its choice wins. The read is remembered, so the next look at the tab uses the file at once. The tried marks go only when
                    // the tab is not shown: with the sheet open the person's choice stands until they act. The facts are read afresh for each
                    // ask: an earlier ask's take may have closed the sheet.
                    const ChoiceFacts f = m_host.facts();
                    const bool tabOpen = f.openTabs.contains(each.tab);
                    const bool shown = f.shownTab == each.tab;
                    if (tabOpen && (f.sheetOpen || !shown)) {
                        for (const QString &hash : std::as_const(chosenHashes)) {
                            if (shown)
                                markBoth(each.tab, hash); // the person's choice holds in both views
                            else
                                m_tried.remove(triedKey(each.tab, each.view, hash));
                        }
                        m_idle.remove(each.tab); // what it found idle before the read may no longer hold (A, B, A, B during one read)
                        done.append({each.token, QStringList()});
                        notRead.append({each.token, QStringList()});
                        continue;
                    }
                    offerNow |= shown && each.offer; // only the opening of a tab asks for the sheet: a switch, a folder or a setting never does
                }
                const QString where = each.tab + QLatin1Char('|') + (each.view == ChoiceView::App ? QLatin1Char('a') : QLatin1Char('g')) + QLatin1Char('|') + chosenHashes.join(QLatin1Char(','));
                if (!appliedTo.contains(where))
                    appliedTo.insert(where, apply(each.tab, each.chosen, resolved, each.view, each.automatic));
                const bool applied = appliedTo.value(where);
                anyApplied |= applied;
                anyDropped |= !applied && !resolved.isEmpty();
                QStringList ok, no;
                for (const QString &hash : wanted)
                    (applied && resolved.contains(hash) ? ok : no) << hash;
                done.append({each.token, ok});
                notRead.append({each.token, no});
            }
            if (!failed.isEmpty() || anyDropped) {
                // The sheet covers the window's notice: while it is open for review the failure is said in the sheet itself.
                const QString said = !failed.isEmpty() ? tr("Could not read %1, so it is not used.").arg(failed.join(QStringLiteral(", ")))
                                                       : tr("The files were read, but the tab changed, so they were not used. Choose again.");
                if (m_host.facts().sheetReview)
                    m_host.sayInSheet(said);
                else
                    m_host.say(said);
            }
            for (int i = 0; i < done.size(); ++i)
                emit finished(done.at(i).second, notRead.at(i).second, done.at(i).first);
            // The line of the read is gone (a failure or the swap's own line replaced it, or nothing was applied).
            endReadingLine();
            // What waited for the files (the Get ready sheet of the tab just opened) opens now, for what is left.
            if (offerNow)
                m_host.offerSheet();
        },
        [this, key](const OnDiskUse &use, int percent) {
            const auto job = m_jobs.constFind(key);
            if (job == m_jobs.constEnd() || job->cancelled)
                return;
            setReading({hashOf(use)}, percent);
            // An automatic read has no sheet to show it: the banner does, as an opened template's does.
            const QString shownTab = m_host.facts().shownTab;
            if (std::any_of(job->asks.begin(), job->asks.end(), [&shownTab](const Ask &each) { return each.automatic && each.tab == shownTab; }))
                sayReading(percent);
        });
}

bool ModelChoice::apply(const QString &tab, const QList<OnDiskUse> &chosen, const Uses &resolved, ChoiceView view, bool announceIt) {
    if (resolved.isEmpty())
        return false; // no file could be read
    if (!m_host.facts().openTabs.contains(tab))
        return false; // the tab was closed meanwhile
    ChoiceDestination *where = dest(view);
    if (!where)
        return false;
    ChoiceRecord record;
    switch (where->take(tab, chosen, resolved, announceIt, &record)) {
    case ChoiceDestination::Take::Refused:
        return false;
    case ChoiceDestination::Take::Unchanged:
        return true;
    case ChoiceDestination::Take::Applied:
        break;
    }
    m_host.changed(false);
    emit applied();
    if (announceIt)
        announce(tab, view, chosen, resolved, record);
    return true;
}

bool ModelChoice::autoPending() const {
    const QString tab = m_host.facts().shownTab;
    if (tab.isEmpty())
        return false;
    for (const Job &job : m_jobs)
        for (const Ask &each : job.asks)
            if (each.automatic && each.tab == tab && !job.cancelled)
                return true;
    return false;
}

bool ModelChoice::offerAfterRead() {
    if (!autoPending())
        return false;
    const QString tab = m_host.facts().shownTab;
    for (Job &job : m_jobs)
        for (Ask &each : job.asks)
            each.offer |= each.automatic && each.tab == tab;
    return true;
}

bool ModelChoice::check(Check kind) {
    const bool cheap = kind == Check::TabShown;
    // Never under a sheet the person is deciding in, nor while a template is opening with its files, nor with nothing shown.
    const ChoiceFacts f = m_host.facts();
    if (f.shownTab.isEmpty() || m_opening || f.sheetOpen || !m_host.autoOn())
        return false;
    const QString tab = f.shownTab;
    // Switching back and forth must not walk the model folders again: a tab whose models were found to need nothing stays so until they change.
    const QByteArray idle = m_host.modelsKey();
    if (cheap && m_idle.value(tab) == idle)
        return false;
    const ChoiceView view = m_app && m_app->shown(tab) ? ChoiceView::App : ChoiceView::Graph;
    QStringList hashes;
    bool waiting = false; // a read of the other view is going: not idle
    for (const OnDiskUse &use : m_host.covered()) {
        const QString hash = hashOf(use);
        if (m_tried.contains(triedKey(tab, view, hash)))
            continue;
        // One automatic swap per model at a time: the other view's read of it is still going, and a second would announce over the first.
        bool pending = false;
        for (const Job &job : std::as_const(m_jobs))
            for (const Ask &each : job.asks)
                if (each.automatic && each.tab == tab && !job.cancelled)
                    for (const OnDiskUse &chosen : each.chosen)
                        pending |= hashOf(chosen) == hash;
        if (pending) {
            waiting = true;
            continue;
        }
        hashes << hash;
    }
    if (hashes.isEmpty()) {
        if (!waiting)
            m_idle.insert(tab, idle);
        return false;
    }
    m_idle.remove(tab);
    for (const QString &hash : std::as_const(hashes))
        markTried(tab, view, hash);
    start(hashes, 0, kind == Check::Offer ? Source::SettingOnOpen : Source::Setting);
    return true;
}

SwapNotice swapNotice(SwapKind kind, const QList<OnDiskUse> &applied) {
    SwapNotice notice;
    if (applied.isEmpty())
        return notice;
    QSet<QString> files; // the title counts the person's files: two models one file stands in for are one file
    for (const OnDiskUse &use : applied)
        files.insert(use.match.path.isEmpty() ? use.match.name : use.match.path);
    const int count = int(files.size());
    notice.title = kind == SwapKind::Opened ? ModelChoice::tr("Opened with your %n files", nullptr, count) : ModelChoice::tr("Using your %n files", nullptr, count);
    QStringList sentences;
    for (const OnDiskUse &use : applied) {
        const QString theirs = use.wanted.value(QStringLiteral("name")).toString();
        QStringList tier{ModelChoice::tr("same model")};
        if (!use.match.tier.isEmpty())
            tier << use.match.tier;
        if (use.match.otherSource)
            tier << ModelChoice::tr("another source");
        notice.lines.append(QVariantMap{{QStringLiteral("mine"), use.match.name},
                                        {QStringLiteral("theirs"), theirs},
                                        {QStringLiteral("minePath"), use.match.path},
                                        {QStringLiteral("tier"), tier.join(QStringLiteral(" · "))}});
        sentences << (kind == SwapKind::Opened ? ModelChoice::tr("Opened with your file %1 in place of %2 (%3).")
                                               : ModelChoice::tr("Used your file %1 in place of %2 (%3)."))
                         .arg(use.match.name, theirs, use.sameModelText());
    }
    notice.text = sentences.join(QLatin1Char('\n'));
    return notice;
}

void ModelChoice::sayChoice(const SwapNotice &notice) {
    if (m_host.sayChoice)
        m_host.sayChoice(notice);
    else
        m_host.say(notice.text);
}

void ModelChoice::announce(const QString &tab, ChoiceView view, const QList<OnDiskUse> &chosen, const Uses &resolved, const ChoiceRecord &record) {
    QList<OnDiskUse> applied;
    for (const OnDiskUse &use : chosen)
        if (resolved.contains(hashOf(use)))
            applied << use;
    if (applied.isEmpty())
        return;
    if (m_host.facts().shownTab != tab)
        return; // not the tab shown: its line would stand on another tab's banner
    sayChoice(swapNotice(SwapKind::Used, applied));
    if (view == ChoiceView::Graph || m_record.tab == tab)
        m_record = {}; // another tab's Undo is left alone by an App-view swap
    if (view == ChoiceView::Graph)
        m_record = record; // the App view keeps its own list of what it uses, with its own Undo
    emit recordChanged();
}

bool ModelChoice::openWith(const QString &key, const QList<OnDiskUse> &uses, const std::function<bool(const Uses &)> &openIt, const QString &original,
                           std::function<void(bool)> opened) {
    m_latestOpen = key; // the last open asked for, also when a caller forgot Workflows::beginOpen: it must never be dropped silently
    if (m_hashing.contains(key))
        return true; // a second click while the files are being read opens no second tab
    // The files' hashes are read first (once, on the worker); the template opens when they are known.
    setHashing(key, true);
    QStringList wanted;
    for (const OnDiskUse &use : uses)
        wanted << hashOf(use);
    setReading(wanted, -1);
    hashUses(
        uses,
        [this, uses, wanted, key, openIt, original, opened](const Uses &resolved, const QStringList &failed) {
            setHashing(key, false);
            clearReading(wanted);
            // A Get ready sheet for another tab may be up (it is modal): the tab opens after it, so the sheet's tab is not changed under it.
            // The caller's `opened` (which view to show) runs when the tab exists, not when the open was asked.
            if (m_latestOpen != key) {
                // Another template or file was opened while these files were read: the person moved on, so this one does not pop up
                // now. The hashes are remembered, so opening it again is at once.
                if (!m_readLine.isEmpty() && m_host.saying() == m_readLine) {
                    m_readLine.clear();
                    m_host.say({});
                }
                if (opened)
                    opened(false);
                return;
            }
            m_host.later([this, uses, resolved, failed, openIt, original, opened] {
                const bool done = finishOpen(uses, resolved, failed, openIt, original);
                if (opened)
                    opened(done);
            });
        },
        [this](const OnDiskUse &use, int percent) {
            setReading({hashOf(use)}, percent);
            sayReading(percent);
        });
    return true;
}

bool ModelChoice::finishOpen(const QList<OnDiskUse> &uses, const Uses &resolved, const QStringList &failed, const std::function<bool(const Uses &)> &openIt,
                             const QString &original) {
    bool opened = false;
    { // a file that could not be read is said below, not tried again by the tab's own check
        const Opening guard(*this);
        opened = openIt(resolved);
    }
    QString tab = m_host.facts().shownTab;
    if (opened && !tab.isEmpty())
        for (const OnDiskUse &use : uses) // asked once: Undo is not undone by the next check, nor is a file that could not be read tried again
            markBoth(tab, hashOf(use));     // the choice covers both views
    if (!failed.isEmpty())
        m_host.say(tr("Opened without %1: the file could not be read.").arg(failed.join(QStringLiteral(", "))));
    tab = m_host.facts().shownTab;
    if (!opened || resolved.isEmpty() || tab.isEmpty())
        return opened;
    QList<OnDiskUse> applied;
    for (const OnDiskUse &use : uses)
        if (resolved.contains(hashOf(use)))
            applied << use;
    if (!failed.isEmpty())
        return true; // the failure line stays the banner's
    const Result<Document> before = Document::loadFile(original);
    sayChoice(swapNotice(SwapKind::Opened, applied));
    m_record = m_graph.took(tab, before ? &before.value() : nullptr);
    emit recordChanged();
    return true;
}

bool ModelChoice::undoable() const { return !m_record.isEmpty() && m_graph.holds(m_record) && !m_host.saying().isEmpty(); }

void ModelChoice::undo() {
    if (!undoable())
        return;
    const ChoiceRecord record = m_record;
    m_record = {};
    if (m_graph.revert(record))
        m_host.changed(true); // what it lacks again, with its Get ready button
}


} // namespace fugue
