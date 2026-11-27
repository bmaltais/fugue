#include "engine/model_inspect.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QProcess>

#include "engine/atomic_file.h"
#include "engine/helper_provider.h"
#include "engine/paths.h"

namespace fugue {

namespace {
constexpr int batchSize = 40; // files on one command line
const QString cacheName = QStringLiteral("model-inspect.json");
} // namespace

QJsonObject EngineModel::toJson() const {
    QJsonObject out{{"recognised", recognised}};
    if (!recognised)
        return out;
    out.insert("version", version);
    out.insert("id", id);
    out.insert("is_dit", isDit);
    out.insert("video", video);
    out.insert("inpaint", inpaint);
    out.insert("latent_channels", latentChannels);
    out.insert("vae_kind", vaeKind);
    return out;
}

EngineModel EngineModel::fromJson(const QJsonObject &object) {
    EngineModel model;
    model.recognised = object.value("recognised").toBool();
    if (!model.recognised)
        return model;
    model.version = object.value("version").toString();
    model.id = object.value("id").toString(); // the engine's family id ("sdxl_inpaint"); a cache of an older Fugue kept a number
    model.isDit = object.value("is_dit").toBool();
    model.video = object.value("video").toBool();
    model.inpaint = object.value("inpaint").toBool();
    model.latentChannels = object.value("latent_channels").toInt();
    model.vaeKind = object.value("vae_kind").toString();
    return model;
}

bool engineDetectsKind(const QString &resourceType) {
    const QString kind = resourceType.section(QLatin1Char('/'), -1);
    return kind == QLatin1String("diffusion-model") || kind == QLatin1String("checkpoint") || kind == QLatin1String("model");
}

ModelInspector::ModelInspector(QString helper, QString cacheFile, int timeoutMs)
    : m_helper(std::move(helper)), m_cacheFile(std::move(cacheFile)), m_timeoutMs(timeoutMs) {
    m_helperStamp = stamp(m_helper);
}

ModelInspector &ModelInspector::shared() {
    static ModelInspector inspector({}, {});
    // Both follow the environment as it is now (FUGUE_HELPER, XDG_CACHE_HOME: a test's fake helper and scratch home).
    inspector.configure(helperProgram(hasNvidiaGpu()), QDir(cacheDir()).filePath(cacheName));
    return inspector;
}

QString ModelInspector::stamp(const QString &program) {
    const QFileInfo info(program);
    return program.isEmpty() || !info.exists()
               ? QString()
               : QStringLiteral("%1:%2:%3").arg(info.absoluteFilePath()).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
}

void ModelInspector::configure(const QString &helper, const QString &cacheFile) {
    QMutexLocker lock(&m_mutex);
    const QString helperStamp = stamp(helper);
    if (helper == m_helper && helperStamp == m_helperStamp && cacheFile == m_cacheFile)
        return;
    m_helper = helper; // another helper may know more, another cache file has other answers
    m_helperStamp = helperStamp;
    m_cacheFile = cacheFile;
    m_entries.clear();
    m_failed.clear();
    m_loaded = false;
}

bool ModelInspector::stampOf(const QString &path, qint64 &size, qint64 &mtime) const {
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    size = info.size();
    mtime = info.lastModified().toMSecsSinceEpoch();
    return true;
}

void ModelInspector::load() {
    if (m_loaded)
        return;
    m_loaded = true;
    if (m_cacheFile.isEmpty())
        return;
    QFile file(m_cacheFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("helper").toString() != m_helperStamp)
        return; // made by another helper
    const QJsonObject files = root.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject object = it.value().toObject();
        Entry entry;
        entry.size = qint64(object.value("size").toDouble());
        entry.mtime = qint64(object.value("mtime").toDouble());
        entry.model = EngineModel::fromJson(object);
        if (!m_entries.contains(it.key()))
            m_entries.insert(it.key(), entry);
    }
}

void ModelInspector::save() {
    if (m_cacheFile.isEmpty())
        return;
    QJsonObject files;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        QJsonObject object = it->model.toJson();
        object.insert("size", double(it->size));
        object.insert("mtime", double(it->mtime));
        files.insert(it.key(), object);
    }
    // Only a failed write is lost: the answers are asked again.
    (void)replaceFile(m_cacheFile, QJsonDocument(QJsonObject{{"helper", m_helperStamp}, {"files", files}}).toJson(QJsonDocument::Compact),
                      QStringLiteral("the model inspection cache"));
}

std::optional<EngineModel> ModelInspector::cached(const QString &path) const {
    QMutexLocker lock(&m_mutex);
    const_cast<ModelInspector *>(this)->load();
    const auto it = m_entries.constFind(path);
    qint64 size = 0, mtime = 0;
    if (it == m_entries.constEnd() || !stampOf(path, size, mtime) || it->size != size || it->mtime != mtime)
        return std::nullopt;
    return it->model;
}

QHash<QString, EngineModel> ModelInspector::ask(const QStringList &paths) {
    QHash<QString, EngineModel> out;
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(m_helper, QStringList{QStringLiteral("--inspect")} + paths);
    if (!process.waitForStarted(5000))
        return out;
    process.closeWriteChannel();
    if (!process.waitForFinished(m_timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return out;
    }
    for (const QByteArray &line : process.readAllStandardOutput().split('\n')) {
        const QJsonObject object = QJsonDocument::fromJson(line).object();
        const QString path = object.value("path").toString();
        if (path.isEmpty() || object.contains("error") || !object.contains("recognised"))
            continue;
        out.insert(path, EngineModel::fromJson(object));
    }
    return out;
}

QHash<QString, EngineModel> ModelInspector::inspect(const QStringList &paths) {
    QMutexLocker lock(&m_mutex);
    load();
    QHash<QString, EngineModel> out;
    QStringList wanted;
    for (const QString &path : paths) {
        qint64 size = 0, mtime = 0;
        if (!stampOf(path, size, mtime))
            continue;
        const auto it = m_entries.constFind(path);
        if (it != m_entries.constEnd() && it->size == size && it->mtime == mtime) {
            out.insert(path, it->model);
            continue;
        }
        const auto failed = m_failed.constFind(path);
        if (failed != m_failed.constEnd() && failed->first == size && failed->second == mtime)
            continue;
        if (!wanted.contains(path))
            wanted << path;
    }
    if (wanted.isEmpty() || m_helper.isEmpty())
        return out;
    bool changed = false;
    for (qsizetype from = 0; from < wanted.size(); from += batchSize) {
        const QStringList batch = wanted.mid(from, batchSize);
        const QHash<QString, EngineModel> answers = ask(batch);
        for (const QString &path : batch) {
            qint64 size = 0, mtime = 0;
            if (!stampOf(path, size, mtime))
                continue;
            const auto it = answers.constFind(path);
            if (it == answers.constEnd()) {
                m_failed.insert(path, {size, mtime});
                continue;
            }
            m_entries.insert(path, Entry{size, mtime, *it});
            out.insert(path, *it);
            changed = true;
        }
    }
    if (changed)
        save();
    return out;
}

} // namespace fugue
