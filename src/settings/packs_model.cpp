#include "settings/packs_model.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include "engine/group_library.h"
#include "engine/pack_loader.h"
#include "engine/pack_secrets.h"
#include "engine/registry.h"
#include "engine/sandbox.h"
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
        m_cancel = true;
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
    const PackReport report = loadPacks(scratch, paths, inheritedPackPaths());
    const TrustStore trust(m_trustFile);
    const PackSecretStore secretStore;
    QVariantList list;
    for (const LoadedPack &pack : report.packs) {
        if (pack.hidden)
            continue;
        QStringList problems;
        for (const Error &error : pack.errors)
            problems << error.message;
        for (const PackProblem &problem : pack.disabled)
            problems << QStringLiteral("%1: %2").arg(problem.node, problem.reason);
        const PackSource source = readPackSource(pack.path);
        // What its programs may do (6.7.1), as the Trust question says it, and whether the person turned the sandbox off.
        sandbox::Needs needs;
        for (const QString &id : pack.nodes)
            if (const NodeType *type = scratch.nodeType(id))
                if (const Result<sandbox::Needs> declared = sandbox::parseNeeds(type->needs))
                    needs = sandbox::merged(needs, *declared);
        const bool sandboxed = pack.community || !trust.isUnsandboxed(pack.id, pack.version);
        const QString secretKey = packSecretKey(pack.id, source.location);
        const QStringList secretsSet = secretStore.namesSet(secretKey);
        QVariantList secrets;
        for (const PackSecretSpec &spec : pack.secrets)
            secrets << QVariantMap{{"name", spec.name}, {"label", spec.label}, {"description", spec.description},
                                   {"set", secretsSet.contains(spec.name)}, {"originRequired", spec.originRequired},
                                   {"origin", secretStore.originOf(secretKey, spec.name)}};
        list << QVariantMap{
            {"id", pack.id}, {"version", pack.version}, {"name", pack.name}, {"path", pack.path},
            {"enabled", pack.enabled}, {"trusted", pack.enabled && trust.isTrusted(pack.id, pack.version)},
            {"removable", QFileInfo(pack.path).absolutePath() == QFileInfo(m_packsFolder).absoluteFilePath()},
            {"nodes", int(pack.nodes.size())}, {"source", source.location}, {"problems", problems},
            {"active", m_registry && pack.enabled && m_registry->hasPackVersion(pack.id, pack.version)},
            {"folder", QFileInfo(pack.path).fileName()}, {"inherited", pack.inherited},
            {"warning", pack.inherited ? inheritedPackWarning() : QString()}, {"secrets", secrets},
            {"sandboxed", sandboxed}, {"needs", sandbox::plainLines(needs, sandboxed)}};
    }
    m_packs = list;
    m_library.clear();
    for (const LibraryGroup &group : libraryGroups(libraryFolder()))
        m_library << QVariantMap{{"id", group.id}, {"name", group.name}, {"version", group.version.toString()}};
    emit packsChanged();
}

QString PacksModel::libraryFolder() const { return m_packsFolder + QStringLiteral("/local"); }

// A library action is quick and runs here, on the window's thread; not while a pack action (with
// its questions) is under way.
void PacksModel::reportLibrary(const QString &message, const Errors &errors) {
    m_message = hasErrors(errors) ? QString() : message;
    m_failure = hasErrors(errors) ? firstProblem(errors) : QString();
    emit messageChanged();
    refresh();
    if (!hasErrors(errors))
        emit packsModified();
}

bool PacksModel::renameLibraryGroup(const QString &id, const QString &name) {
    if (m_busy)
        return false;
    const Registry none;
    const Result<QString> renamed = renameInLibrary(id, name, m_registry ? *m_registry : none, libraryFolder());
    const QString message =
        renamed ? tr("Renamed %1 to %2. Workflows that use it keep their own copy.").arg(id, *renamed) : QString();
    reportLibrary(message, renamed ? Errors() : renamed.errors());
    if (renamed)
        emit libraryGroupRenamed(id, *renamed);
    return bool(renamed);
}

bool PacksModel::removeLibraryGroup(const QString &id) {
    if (m_busy)
        return false;
    const Errors removed = removeFromLibrary(id, libraryFolder());
    reportLibrary(tr("Removed %1 from the library. Workflows that use it keep their own copy.").arg(id), removed);
    return !hasErrors(removed);
}

// The pack's own entry in the list: its secrets and the key they are stored under.
// `path` is the pack's own folder (the list's "path"): with it the pack is found by where it is, so two packs of one id from
// different sources never share a key (#402); without it the first enabled pack of that id is taken.
bool PacksModel::setSecret(const QString &packId, const QString &name, const QString &value, const QString &origin, const QString &path) {
    PackSecretStore store;
    bool declared = false, required = false;
    QString key;
    for (const QVariant &entry : std::as_const(m_packs)) {
        if (entry.toMap().value("id") != packId || !entry.toMap().value("enabled").toBool())
            continue;
        if (!path.isEmpty() && entry.toMap().value("path").toString() != path)
            continue;
        if (declared)
            break;
        for (const QVariant &secret : entry.toMap().value("secrets").toList())
            if (secret.toMap().value("name") == name) {
                declared = true;
                required = secret.toMap().value("originRequired").toBool();
                key = packSecretKey(packId, entry.toMap().value("source").toString());
            }
    }
    Errors errors;
    if (!declared)
        errors << makeError(QStringLiteral("usage"), tr("%1 does not declare a secret named %2.").arg(packId, name));
    else
        errors = store.set(key, name, value, origin, required); // the same rules as the command line
    m_message = hasErrors(errors) ? QString() : tr("Stored %1 of %2.").arg(name, packId);
    m_failure = hasErrors(errors) ? firstProblem(errors) : QString();
    emit messageChanged();
    refresh();
    return !hasErrors(errors);
}

bool PacksModel::clearSecret(const QString &packId, const QString &name, const QString &path) {
    PackSecretStore store;
    QString key = packId;
    for (const QVariant &entry : std::as_const(m_packs))
        if (entry.toMap().value("id") == packId && (path.isEmpty() || entry.toMap().value("path").toString() == path)) {
            key = packSecretKey(packId, entry.toMap().value("source").toString());
            if (!path.isEmpty())
                break;
        }
    const Errors errors = store.clear(key, name);
    m_message = hasErrors(errors) ? QString() : tr("Cleared %1 of %2.").arg(name, packId);
    m_failure = hasErrors(errors) ? firstProblem(errors) : QString();
    emit messageChanged();
    refresh();
    return !hasErrors(errors);
}

Answer PacksModel::ask(const QString &kind, const PackSummary &pack) {
    QVariantMap question{
        {"kind", kind}, {"id", pack.id}, {"name", pack.name.isEmpty() ? pack.id : pack.name}, {"version", pack.version}, {"oldVersion", pack.oldVersion},
        {"source", pack.source.location.isEmpty() ? QString()
                   : pack.source.ref.isEmpty() ? pack.source.location
                                               : QStringLiteral("%1 at %2").arg(pack.source.location, pack.source.ref)},
        {"runtime", pack.runtimeKinds}, {"permissions", pack.permissions},
        // A setup or a run of a version the person turned the sandbox off for runs as them (6.7.1): said where it is asked.
        {"sandboxed", pack.community || !TrustStore().isUnsandboxed(pack.id, pack.version)}, {"secrets", pack.secrets}, {"secretsSet", pack.secretsSet}, {"secretsOtherSource", pack.secretsOtherSource}, {"nodes", int(pack.nodes.size())},
        {"command", displayCommand(pack.setupCommand)}, {"description", pack.setupDescription}, {"folder", pack.folder},
        // A pack of the Default project runs from, and writes into, the Default's folders: said where it is trusted.
        {"inheritedWarning", isInheritedPack(pack.id, pack.version) ? inheritedPackWarning() : QString()}};
    {
        QMutexLocker lock(&m_mutex);
        if (m_closing)
            return Answer::No;
        m_answer = -1;
    }
    QMetaObject::invokeMethod(this, [this, question] {
        m_question = question;
        m_questionReady = false;
        emit questionChanged();
        QTimer::singleShot(600, this, [this] {
            if (!m_question.isEmpty() && !m_questionReady) {
                m_questionReady = true;
                emit questionChanged();
            }
        });
    }, Qt::QueuedConnection);
    QMutexLocker lock(&m_mutex);
    while (m_answer < 0)
        m_answered.wait(&m_mutex);
    if (Answer(m_answer) != Answer::Yes)
        m_sawNo = true;
    return Answer(m_answer);
}

void PacksModel::cancel() {
    m_cancel = true;
    if (!m_question.isEmpty()) {
        m_questionReady = true;
        answer(false);
    }
}

void PacksModel::answer(bool yes) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_answer >= 0 || m_question.isEmpty() || !m_questionReady)
            return;
        m_answer = int(yes ? Answer::Yes : Answer::No);
    }
    m_question.clear();
    m_questionReady = false;
    emit questionChanged();
    m_answered.wakeAll();
}

PackPrompts PacksModel::prompts() {
    PackPrompts prompts;
    prompts.cancelled = [this] { return m_cancel.load(); };
    prompts.install = [this](const PackSummary &pack) { return ask(pack.updating ? "update" : "install", pack); };
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
        m_busy = false;
        m_question.clear();
        emit messageChanged();
        emit questionChanged();
        emit busyChanged();
        refresh();
        if (changed)
            emit packsModified();
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
    m_cancel = false;
    m_sawNo = false;
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
        QString text = tr("Installed %1 %2. Its nodes are in node search; trust it before they run.").arg(added->pack.id, added->pack.version);
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
        finish(changed->isEmpty() ? (m_sawNo ? tr("Nothing was updated; you said no.") : tr("Nothing to update.")) : tr("%n packs updated.", nullptr, int(changed->size())),
               changed.warnings(), !changed->isEmpty());
    });
}

void PacksModel::remove(const QString &id) {
    start([this, id] {
        TrustStore trust(m_trustFile);
        const Result<PackSummary> removed = removePack(id, m_packsFolder, trust, prompts());
        finish(removed ? tr("Removed %1. Workflows that use its nodes keep them as placeholders.").arg(id) : QString(),
               removed ? removed.warnings() : removed.errors(), bool(removed));
    });
}

void PacksModel::trust(const QString &id) {
    start([this, id] {
        TrustStore trust(m_trustFile);
        const Result<PackSummary> done = trustPack(id, m_packsFolder, trust, prompts());
        finish(done ? tr("Trusted %1 %2.").arg(done->id, done->version) : QString(), done ? Errors() : done.errors(), bool(done));
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
