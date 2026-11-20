#include "model_downloads.h"

#include <QFileInfo>

#include <algorithm>

namespace fugue {

ModelDownloads::ModelDownloads(QObject *parent) : QObject(parent) {
    connect(&m_queue, &DownloadQueue::started, this, [this] { notifyChanged(true); });
    connect(&m_queue, &DownloadQueue::progress, this, [this] { notifyChanged(false); });
    connect(&m_queue, &DownloadQueue::fileFinished, this, [this](int, const Errors &errors) {
        if (errors.isEmpty())
            ++m_arrived;
    });
    connect(&m_queue, &DownloadQueue::finished, this, [this](const Errors &errors) {
        const int arrived = m_arrived;
        m_arrived = 0;
        QStringList failed;
        for (const Error &error : errors)
            if (error.code != QLatin1String("cancelled"))
                failed << error.message;
        m_failure = failed.join(QLatin1Char(' '));
        m_failedWhat = m_failure.isEmpty() ? QString() : m_what;
        m_what.clear();
        emit changed();
        emit finished(arrived, errors);
    });
}

QString ModelDownloads::sizeText(qint64 bytes) {
    return bytes >= 1000000000 ? tr("%1 GB").arg(double(bytes) / 1e9, 0, 'f', 1)
                               : tr("%1 MB").arg(std::max<qint64>(1, (bytes + 500000) / 1000000));
}

QString ModelDownloads::label() const {
    if (!busy())
        return {};
    const QString shown = QFileInfo(m_queue.targets().at(m_queue.index()).fileName()).fileName();
    return m_queue.targets().size() > 1
               ? tr("%1, %2 of %3").arg(shown).arg(m_queue.index() + 1).arg(m_queue.targets().size())
               : shown;
}

QString ModelDownloads::progressText() const {
    if (!busy() || m_queue.total() < 0)
        return {};
    return tr("%1 of %2").arg(sizeText(m_queue.received()), sizeText(m_queue.total()));
}

double ModelDownloads::fraction() const {
    if (!busy() || m_queue.total() <= 0)
        return -1;
    return double(m_queue.received()) / double(m_queue.total());
}

void ModelDownloads::add(const QList<DownloadTarget> &targets, const QString &what) {
    if (targets.isEmpty())
        return;
    if (!busy()) {
        m_what = what;
        m_failure.clear();
        m_failedWhat.clear();
    }
    m_queue.add(targets);
    notifyChanged(true);
}

void ModelDownloads::notifyChanged(bool now) {
    if (!now && m_told.isValid() && m_told.elapsed() < 100)
        return;
    m_told.start();
    emit changed();
}

} // namespace fugue
