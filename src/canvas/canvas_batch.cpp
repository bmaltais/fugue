#include "canvas/canvas_batch.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QUrl>

#include <algorithm>

#include "canvas/canvas_actions.h"
#include "engine/atomic_file.h"
#include "run/run_service.h"

namespace fugue {

namespace {

QString localPath(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    return url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
}

} // namespace

CanvasBatch::CanvasBatch(RunService *service, CanvasActions *actions, QObject *parent) : QObject(parent), m_service(service), m_actions(actions) {
    connect(m_service, &RunService::event, this, &CanvasBatch::onEvent);
}

QVariantList CanvasBatch::actions() const {
    QVariantList out;
    for (const QVariant &value : m_actions->actions()) {
        const QVariantMap row = value.toMap();
        if (row.value("broken").toBool() || row.value("selection").toString() == QLatin1String("bounds"))
            continue;
        out << QVariantMap{{"id", row.value("id")}, {"name", row.value("name")}, {"description", row.value("description")},
                           {"needsPrompt", row.value("needsPrompt")}, {"missing", row.value("missing")}};
    }
    return out;
}

int CanvasBatch::pictureCount(const QString &folder) const {
    const Result<QStringList> files = canvas::batchPictures(localPath(folder));
    return files ? int(files->size()) : -1;
}

QVariantMap CanvasBatch::start(const QVariantMap &request) {
    const auto refuse = [](const QString &why) { return QVariantMap{{"ok", false}, {"reason", why}}; };
    if (m_busy)
        return refuse(tr("A batch is already running: wait for it, or Cancel it."));
    const Result<canvas::BatchAction> action = m_actions->batchAction(request.value("action").toString());
    if (!action)
        return refuse(action.error().message);
    canvas::BatchSpec spec;
    spec.action = action.value();
    spec.inFolder = localPath(request.value("inFolder").toString());
    spec.outFolder = localPath(request.value("outFolder").toString());
    if (spec.inFolder.isEmpty())
        return refuse(tr("Choose the folder of pictures."));
    if (spec.outFolder.isEmpty())
        return refuse(tr("Choose the folder to write the results to."));
    spec.picture.format = canvas::normalFormat(request.value("format", QStringLiteral("png")).toString(), true);
    if (spec.picture.format.isEmpty())
        spec.picture.format = QStringLiteral("png");
    spec.picture.quality = std::clamp(request.value("quality", 92).toInt(), 1, 100);
    spec.picture.lossless = false;
    spec.prompt = request.value("prompt").toString();
    if (const Errors problems = canvas::checkBatch(spec); hasErrors(problems))
        return refuse(spec.action.action.needsPrompt() && spec.prompt.trimmed().isEmpty() ? tr("Type what to put in the pictures, then Run.") : problems.first().message);
    const Result<QStringList> files = canvas::batchPictures(spec.inFolder);
    if (!files)
        return refuse(files.error().message);
    if (files->isEmpty())
        return refuse(tr("That folder has no pictures (.png, .jpg, .webp, .bmp, .tif or .ora)."));
    if (const Errors made = makeFolder(spec.outFolder); hasErrors(made))
        return refuse(made.first().message);

    m_spec = spec;
    m_files = files.value();
    m_taken.clear();
    m_results.clear();
    for (const QString &file : std::as_const(m_files))
        m_results << QVariantMap{{"name", QFileInfo(file).fileName()}, {"status", QStringLiteral("waiting")}, {"output", QString()}, {"error", QString()}};
    m_index = -1;
    m_done = 0;
    m_busy = true;
    m_finished = false;
    m_cancelling = false;
    m_summary.clear();
    m_progress = -1;
    emit resultsChanged();
    emit stateChanged();
    next();
    return {{"ok", true}};
}

void CanvasBatch::setStatus(int index, const QString &status) {
    QVariantMap row = m_results.at(index).toMap();
    row.insert("status", status);
    m_results[index] = row;
    emit resultsChanged();
}

void CanvasBatch::endItem(int index, const QString &status, const QString &output, const QString &error) {
    QVariantMap row = m_results.at(index).toMap();
    row.insert("status", status);
    row.insert("output", output);
    row.insert("error", error);
    m_results[index] = row;
    ++m_done;
    m_progress = -1;
    emit resultsChanged();
    emit progressChanged();
    emit stateChanged();
    emit itemDone(index, status);
}

void CanvasBatch::next() {
    canvas::CanvasStore store = canvas::CanvasStore::forSession();
    while (++m_index < int(m_files.size())) {
        if (m_cancelling) {
            endItem(m_index, QStringLiteral("cancelled"), {}, tr("Not started: the batch was cancelled."));
            continue;
        }
        const QString &file = m_files.at(m_index);
        const Result<canvas::BatchPrepared> prepared = canvas::batchPrepare(m_spec, file, store);
        if (!prepared) {
            endItem(m_index, QStringLiteral("failed"), {}, prepared.error().message);
            continue;
        }
        m_prepared = prepared.value();
        m_errors.clear();
        RunRequest run;
        run.document = m_prepared.bound;
        run.workflow = m_spec.action.file;
        m_run = m_service->start(run);
        setStatus(m_index, QStringLiteral("running"));
        return;
    }
    conclude();
}

void CanvasBatch::onEvent(const QString &run, const QJsonObject &event) {
    if (!m_busy || run != m_run)
        return;
    const QString kind = event.value("event").toString();
    if (kind == QLatin1String("node.progress")) {
        m_progress = std::clamp(event.value("value").toDouble(), 0.0, 1.0);
        emit progressChanged();
    } else if (kind == QLatin1String("node.error")) {
        m_errors << Error::fromJson(event.value("error").toObject());
    } else if (kind == QLatin1String("run.done")) {
        for (const QJsonValue &value : event.value("errors").toArray())
            m_errors << Error::fromJson(value.toObject());
        const QString status = event.value("status").toString();
        const int index = m_index;
        m_run.clear();
        canvas::CanvasStore store = canvas::CanvasStore::forSession();
        if (status == QLatin1String("ok")) {
            const Result<QString> output = canvas::batchFinish(m_spec, m_prepared, m_files.at(index), m_taken, store);
            if (output) {
                endItem(index, QStringLiteral("ok"), output.value(), {});
            } else {
                canvas::batchDiscard(m_spec, m_prepared, store);
                endItem(index, QStringLiteral("failed"), {}, output.error().message);
            }
        } else if (status == QLatin1String("cancelled")) {
            canvas::batchDiscard(m_spec, m_prepared, store);
            m_cancelling = true;
            endItem(index, QStringLiteral("cancelled"), {}, tr("Cancelled."));
        } else {
            canvas::batchDiscard(m_spec, m_prepared, store);
            const auto hard = std::find_if(m_errors.cbegin(), m_errors.cend(), [](const Error &e) { return e.severity == Error::Severity::Error; });
            const Error first = hard != m_errors.cend() ? *hard : makeError(QStringLiteral("node_failed"), tr("%1 did not finish.").arg(m_spec.action.action.name));
            endItem(index, QStringLiteral("failed"), {}, CanvasActions::plainWords(first, m_spec.action.action.name));
        }
        next();
    }
}

void CanvasBatch::cancel() {
    if (!m_busy)
        return;
    m_cancelling = true;
    if (!m_run.isEmpty())
        m_service->cancel(m_run);
}

void CanvasBatch::conclude() {
    int ok = 0, failed = 0, cancelled = 0;
    for (const QVariant &value : std::as_const(m_results)) {
        const QString status = value.toMap().value("status").toString();
        ok += status == QLatin1String("ok");
        failed += status == QLatin1String("failed");
        cancelled += status == QLatin1String("cancelled");
    }
    QStringList parts{tr("%n pictures done", nullptr, ok)};
    if (failed > 0)
        parts << tr("%n failed", nullptr, failed);
    if (cancelled > 0)
        parts << tr("%n cancelled", nullptr, cancelled);
    m_summary = parts.join(QStringLiteral(", "));
    m_busy = false;
    m_finished = true;
    m_progress = -1;
    emit progressChanged();
    emit stateChanged();
}

void CanvasBatch::reset() {
    if (m_busy)
        return;
    m_results.clear();
    m_files.clear();
    m_summary.clear();
    m_finished = false;
    m_done = 0;
    emit resultsChanged();
    emit stateChanged();
}

} // namespace fugue
