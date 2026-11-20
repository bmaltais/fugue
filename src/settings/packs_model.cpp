#include "settings/packs_model.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "engine/trust_store.h"

namespace fugue {

namespace {

QString firstProblem(const Errors &errors) {
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error)
            return error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint;
    return errors.isEmpty() ? QString() : errors.first().message;
}

} // namespace

PacksModel::PacksModel(const Registry *registry, const QString &packsFolder, const QString &trustFile, QObject *parent)
    : QObject(parent), m_registry(registry), m_packsFolder(packsFolder.isEmpty() ? packsDir() : packsFolder),
      m_trustFile(trustFile.isEmpty() ? TrustStore::defaultPath() : trustFile) {
    refresh();
}

PacksModel::~PacksModel() {
    {
        QMutexLocker lock(&m_mutex);
        m_closing = true;
        m_answer = int(Answer::No); // a question still open is a no
    }
    m_answered.wakeAll();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void PacksModel::refresh() {
    Registry scratch;
    QStringList paths = packSearchPaths();
    if (!paths.contains(m_packsFolder))
        paths.prepend(m_packsFolder);
    const PackReport report = loadPacks(scratch, paths);
    const TrustStore trust(m_trustFile);
    QVariantList list;
    for (const LoadedPack &pack : report.packs) {
        QStringList problems;
        for (const Error &error : pack.errors)
            problems << error.message;
        for (const PackProblem &problem : pack.disabled)
            problems << QStringLiteral("%1: %2").arg(problem.node, problem.reason);
        const PackSource source = readPackSource(pack.path);
        list << QVariantMap{
            {"id", pack.id}, {"version", pack.version}, {"name", pack.name}, {"path", pack.path},
            {"enabled", pack.enabled}, {"trusted", pack.enabled && trust.isTrusted(pack.id, pack.version)},
            {"removable", QFileInfo(pack.path).absolutePath() == QFileInfo(m_packsFolder).absoluteFilePath()},
            {"nodes", int(pack.nodes.size())}, {"source", source.location}, {"problems", problems},
            {"active", m_registry && pack.enabled && m_registry->packVersion(pack.id) == pack.version}};
    }
    m_packs = list;
    emit packsChanged();
}

Answer PacksModel::ask(const QString &kind, const PackSummary &pack) {
    QVariantMap question{
        {"kind", kind}, {"id", pack.id}, {"name", pack.name}, {"version", pack.version}, {"oldVersion", pack.oldVersion},
        {"source", pack.source.location.isEmpty() ? QString()
                   : pack.source.ref.isEmpty() ? pack.source.location
                                               : QStringLiteral("%1 at %2").arg(pack.source.location, pack.source.ref)},
        {"runtime", pack.runtimeKinds}, {"permissions", pack.permissions}, {"nodes", int(pack.nodes.size())},
        {"command", displayCommand(pack.setupCommand)}, {"description", pack.setupDescription}, {"folder", pack.folder}};
    {
        QMutexLocker lock(&m_mutex);
        if (m_closing)
            return Answer::No;
        m_answer = -1;
    }
    QMetaObject::invokeMethod(this, [this, question] {
        m_question = question;
        emit questionChanged();
    }, Qt::QueuedConnection);
    QMutexLocker lock(&m_mutex);
    while (m_answer < 0)
        m_answered.wait(&m_mutex);
    return Answer(m_answer);
}

void PacksModel::answer(bool yes) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_answer >= 0 || m_question.isEmpty())
            return;
        m_answer = int(yes ? Answer::Yes : Answer::No);
    }
    m_question.clear();
    emit questionChanged();
    m_answered.wakeAll();
}

PackPrompts PacksModel::prompts() {
    PackPrompts prompts;
    prompts.install = [this](const PackSummary &pack) { return ask(pack.oldVersion.isEmpty() ? "install" : "update", pack); };
    prompts.remove = [this](const PackSummary &pack) { return ask("remove", pack); };
    prompts.trust = [this](const PackSummary &pack) { return ask("trust", pack); };
    prompts.runSetup = [this](const PackSummary &pack) { return ask("setup", pack); };
    prompts.log = [this](const QString &line) {
        QMetaObject::invokeMethod(this, [this, line] {
            m_log << line;
            if (m_log.size() > 200)
                m_log = m_log.mid(m_log.size() - 200);
            emit logChanged();
        }, Qt::QueuedConnection);
    };
    return prompts;
}

void PacksModel::finish(const QString &message, const Errors &errors, bool changed) {
    QMetaObject::invokeMethod(this, [this, message, errors, changed] {
        m_message = hasErrors(errors) ? QString() : message;
        m_failure = hasErrors(errors) ? firstProblem(errors) : QString();
        for (const Error &error : errors)
            if (error.severity == Error::Severity::Warning && !hasErrors(errors))
                m_failure += (m_failure.isEmpty() ? QString() : QStringLiteral("\n")) + error.message;
        if (changed)
            m_restartNeeded = true;
        m_busy = false;
        m_question.clear();
        emit messageChanged();
        emit questionChanged();
        emit busyChanged();
        refresh();
    }, Qt::QueuedConnection);
}

template <typename Work> void PacksModel::start(Work work) {
    if (m_busy)
        return;
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    m_busy = true;
    m_log.clear();
    m_message.clear();
    m_failure.clear();
    emit busyChanged();
    emit logChanged();
    emit messageChanged();
    m_thread = QThread::create(work);
    m_thread->start();
}

void PacksModel::add(const QString &location, const QString &ref) {
    start([this, location, ref] {
        TrustStore trust(m_trustFile);
        const Result<PackAdded> added = addPack(location.trimmed(), ref.trimmed(), m_packsFolder, trust, prompts());
        if (!added) {
            finish({}, added.errors(), false);
            return;
        }
        QString text = tr("Installed %1 %2. Trust it before it runs; restart Fugue to use its nodes.").arg(added->pack.id, added->pack.version);
        if (added->setupSkipped)
            text += QLatin1Char(' ') + tr("Its setup was not run.");
        finish(text, added.warnings(), true);
    });
}

void PacksModel::update(const QString &id) {
    start([this, id] {
        TrustStore trust(m_trustFile);
        const Result<QList<PackAdded>> changed = updatePacks(id, {}, m_packsFolder, trust, prompts());
        if (!changed) {
            finish({}, changed.errors(), false);
            return;
        }
        finish(changed->isEmpty() ? tr("Nothing to update.") : tr("Updated %n pack(s). Restart Fugue to use them.", nullptr, int(changed->size())),
               changed.warnings(), !changed->isEmpty());
    });
}

void PacksModel::remove(const QString &id) {
    start([this, id] {
        TrustStore trust(m_trustFile);
        const Result<PackSummary> removed = removePack(id, m_packsFolder, trust, prompts());
        finish(removed ? tr("Removed %1. Restart Fugue to stop using its nodes.").arg(id) : QString(), removed ? Errors() : removed.errors(), bool(removed));
    });
}

void PacksModel::trust(const QString &id) {
    start([this, id] {
        TrustStore trust(m_trustFile);
        const Result<PackSummary> done = trustPack(id, m_packsFolder, trust, prompts());
        finish(done ? tr("Trusted %1 %2.").arg(done->id, done->version) : QString(), done ? Errors() : done.errors(), false);
    });
}

bool PacksModel::waitIdle(int msec) {
    QElapsedTimer timer;
    timer.start();
    while (m_busy && timer.elapsed() < msec) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return !m_busy;
}

} // namespace fugue
