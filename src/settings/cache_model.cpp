#include "settings/cache_model.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThread>

#include "engine/model_download.h"

namespace fugue {

namespace {

QString titleOf(CacheKind kind) {
    switch (kind) {
    case CacheKind::Results: return QObject::tr("Results");
    case CacheKind::Thumbnails: return QObject::tr("Image previews");
    case CacheKind::Runs: return QObject::tr("Run history");
    }
    return {};
}

// What clearing a kind loses, in the words a person uses.
QString lostBy(CacheKind kind) {
    switch (kind) {
    case CacheKind::Results:
        return QObject::tr("Every node that was run will run again the next time, so what took minutes takes minutes again. "
                           "Frozen results and your saved pictures are kept.");
    case CacheKind::Thumbnails:
        return QObject::tr("Nothing is lost for good. Pictures on the canvas show again after the next run, "
                           "which is quick for nodes whose results are kept.");
    case CacheKind::Runs:
        return QObject::tr("The history of past runs (their events and working files) is gone. Results, saved pictures "
                           "and any run that is going or waiting are kept.");
    }
    return {};
}

QString day(const QDateTime &time) { return time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd")); }

QString detailOf(const KindUsage &usage) {
    QStringList parts;
    switch (usage.kind) {
    case CacheKind::Results:
        parts << QObject::tr("%n results", nullptr, usage.items);
        parts << (usage.limit > 0 ? QObject::tr("limit %1").arg(sizeText(usage.limit)) : QObject::tr("no limit"));
        break;
    case CacheKind::Thumbnails:
        parts << QObject::tr("%n pictures", nullptr, usage.items);
        parts << (usage.limit > 0 ? QObject::tr("limit %1").arg(sizeText(usage.limit)) : QObject::tr("no limit"));
        break;
    case CacheKind::Runs:
        parts << QObject::tr("%n runs", nullptr, usage.items);
        break;
    }
    if (usage.items > 0 && usage.oldest.isValid())
        parts << QObject::tr("oldest %1").arg(day(usage.oldest));
    if (usage.protectedItems > 0)
        parts << (usage.kind == CacheKind::Results ? QObject::tr("%n frozen", nullptr, usage.protectedItems)
                                                   : QObject::tr("%n going or waiting", nullptr, usage.protectedItems));
    return parts.join(QStringLiteral(" · "));
}

} // namespace

CacheModel::CacheModel(QObject *parent) : CacheModel(CacheFolders::standard(), parent) {}

CacheModel::CacheModel(CacheFolders folders, QObject *parent) : QObject(parent), m_folders(std::move(folders)) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &CacheModel::cleanAutomatically);
}

CacheModel::~CacheModel() { waitIdle(); }

void CacheModel::waitIdle() {
    while (!m_threads.isEmpty()) {
        QThread *thread = m_threads.takeFirst();
        thread->wait();
        QCoreApplication::sendPostedEvents(this);
    }
    QCoreApplication::sendPostedEvents(this);
}

// Runs `work()` on a thread of its own and `done(result)` back here.
template <class Work, class Done>
void CacheModel::work(Work job, Done done) {
    ++m_working;
    if (m_working == 1)
        emit busyChanged();
    const QPointer<CacheModel> self(this);
    QThread *thread = QThread::create([self, job, done]() mutable {
        auto result = job();
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, done, result]() mutable {
            if (!self)
                return;
            done(result);
            if (--self->m_working == 0)
                emit self->busyChanged();
        }, Qt::QueuedConnection);
    });
    m_threads.append(thread);
    connect(thread, &QThread::finished, this, [this, thread] {
        m_threads.removeOne(thread);
        thread->deleteLater();
    });
    thread->start();
}

void CacheModel::refresh() {
    const CacheFolders folders = m_folders;
    work([folders] { return CacheManager(folders).status(CachePolicy::current()); },
         [this](const CacheStatus &status) {
             QVariantList list;
             for (const KindUsage &usage : status.kinds)
                 list.append(QVariantMap{{"kind", cacheKindName(usage.kind)},
                                         {"title", titleOf(usage.kind)},
                                         {"bytes", usage.bytes},
                                         {"size", sizeText(usage.bytes)},
                                         {"detail", detailOf(usage)},
                                         {"items", usage.items},
                                         {"protectedItems", usage.protectedItems},
                                         {"canClear", usage.items > usage.protectedItems}});
             m_kinds = list;
             m_total = sizeText(status.totalBytes());
             emit changed();
             if (!m_question.isEmpty()) { // asked before the sizes were known
                 for (const QVariant &entry : std::as_const(m_kinds))
                     if (entry.toMap().value("kind") == m_question.value("kind"))
                         m_question.insert("size", entry.toMap().value("size"));
                 emit questionChanged();
             }
         });
}

void CacheModel::ask(const QString &kind) {
    const std::optional<CacheKind> parsed = parseCacheKind(kind);
    if (!parsed)
        return;
    QVariantMap question{{"kind", kind}, {"title", titleOf(*parsed)}, {"what", lostBy(*parsed)}, {"size", QString()}};
    for (const QVariant &entry : std::as_const(m_kinds))
        if (entry.toMap().value("kind").toString() == kind)
            question.insert("size", entry.toMap().value("size"));
    m_question = question;
    emit questionChanged();
    if (m_kinds.isEmpty())
        refresh(); // the size shows once it is known
}

void CacheModel::answer(bool yes) {
    const QString kind = m_question.value("kind").toString();
    m_question.clear();
    emit questionChanged();
    const std::optional<CacheKind> parsed = parseCacheKind(kind);
    if (!yes || !parsed)
        return;
    ClearRequest request;
    request.kinds = {*parsed};
    const CacheFolders folders = m_folders;
    work([folders, request] { return CacheManager(folders).clear(request); },
         [this, kind, parsed](const Result<ClearReport> &report) {
             if (!report) {
                 setMessage(report.errors().first().message);
             } else {
                 const KindOutcome &outcome = report.value().kinds.value(0);
                 if (!outcome.skipped.isEmpty())
                     setMessage(tr("%1 was left alone: %2. Try again when the run has ended.").arg(titleOf(*parsed), outcome.skipped));
                 else
                     setMessage(tr("Cleared %1 of %2.").arg(sizeText(outcome.bytes), titleOf(*parsed).toLower()));
                 emit cleared(kind, outcome.bytes);
             }
             refresh();
         });
}

void CacheModel::setMessage(const QString &text) {
    m_message = text;
    emit messageChanged();
}

void CacheModel::startAutomatic(int firstMs, int intervalMs) {
    m_intervalMs = intervalMs;
    m_timer.start(firstMs);
}

void CacheModel::cleanAutomatically() {
    const CacheFolders folders = m_folders;
    // cleanup() takes the engine lock and leaves results and previews alone when a run holds it.
    work([folders] { return CacheManager(folders).cleanup(CachePolicy::current()); },
         [this](const ClearReport &report) {
             if (report.bytes() > 0)
                 refresh();
             if (m_intervalMs > 0)
                 m_timer.start(m_intervalMs);
         });
}

} // namespace fugue
