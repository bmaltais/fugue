#include "media/media_library.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMutexLocker>

#include "engine/run_folder.h"
#include "media/media_items.h"
#include "media/thumbnail_cache.h"

namespace fugue {

namespace {

class Provider : public QQuickImageProvider {
public:
    Provider(const MediaLibrary *library, QString dir)
        : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading),
          m_library(library), m_dir(std::move(dir)) {}

    // The id is "<box>/<token>".
    QImage requestImage(const QString &id, QSize *size, const QSize &) override {
        const qsizetype slash = id.indexOf('/');
        const int box = slash > 0 ? qBound(32, id.left(slash).toInt(), 1024) : 256;
        const QString path = m_library->pathOfToken(id.mid(slash + 1));
        const QImage image = path.isEmpty() ? QImage() : thumbnail(m_dir, path, box);
        if (size)
            *size = image.size();
        return image;
    }

private:
    const MediaLibrary *m_library;
    QString m_dir;
};

QString humanBytes(qint64 bytes) { return QLocale().formattedDataSize(bytes, 1); }

} // namespace

MediaLibrary::MediaLibrary(QString runsRoot, QString thumbDir, QObject *parent)
    : QObject(parent), m_runsRoot(runsRoot.isEmpty() ? RunFolder::defaultRoot() : std::move(runsRoot)),
      m_thumbDir(thumbDir.isEmpty() ? defaultThumbnailDir() : std::move(thumbDir)),
      m_openUrl([](const QUrl &url) { QDesktopServices::openUrl(url); }) {}

QString MediaLibrary::thumbSource(const QString &path, int box) const {
    const QString token = QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex());
    {
        QMutexLocker locker(&m_lock);
        m_tokens.insert(token, path);
    }
    return QStringLiteral("image://%1/%2/%3").arg(QLatin1String(providerName)).arg(box).arg(token);
}

QString MediaLibrary::pathOfToken(const QString &token) const {
    QMutexLocker locker(&m_lock);
    return m_tokens.value(token);
}

QQuickImageProvider *MediaLibrary::makeProvider() const { return new Provider(this, m_thumbDir); }

QVariantList MediaLibrary::resultsOf(const QString &workflow) const { return rowsOf(mediaOfWorkflow(m_runsRoot, workflow)); }

QVariantList MediaLibrary::resultsOfRun(const QString &run) const { return rowsOf(mediaOfRun(m_runsRoot, run)); }

QVariantList MediaLibrary::rowsOf(const QList<MediaItem> &items) const {
    QVariantList rows;
    for (const MediaItem &item : items) {
        rows.append(QVariantMap{{"path", item.path},
                                {"url", QUrl::fromLocalFile(item.path)},
                                {"kind", item.kind},
                                {"title", item.name},
                                {"group", QLocale().toString(item.time.toLocalTime(), QLocale::ShortFormat)},
                                {"run", item.run},
                                {"node", item.node},
                                {"thumb", item.kind == QLatin1String("image") ? thumbSource(item.path) : QString()}});
    }
    return rows;
}

QVariantList MediaLibrary::detailsOf(const QString &path, const QString &run, const QString &node) const {
    QVariantList rows;
    auto add = [&rows](const QString &label, const QVariant &value) {
        if (!value.toString().isEmpty())
            rows.append(QVariantMap{{"label", label}, {"value", value}});
    };
    const QFileInfo info(path);
    add(tr("File"), info.fileName());
    add(tr("Folder"), info.absolutePath());
    if (info.isFile())
        add(tr("Size"), humanBytes(info.size()));
    const QJsonObject embedded = embeddedSummary(path);
    add(tr("Prompt"), embedded.value("prompt").toString());
    if (embedded.contains("seed"))
        add(tr("Seed"), QString::number(embedded.value("seed").toDouble(), 'f', 0));
    add(tr("Model"), embedded.value("model").toString());
    add(tr("Run"), run.isEmpty() ? QString() : QLocale().toString(runStartOf(run).toLocalTime(), QLocale::ShortFormat));
    add(tr("Node"), node);
    return rows;
}

void MediaLibrary::openFile(const QString &path) const {
    if (m_openUrl && QFileInfo(path).isFile())
        m_openUrl(QUrl::fromLocalFile(path));
}

void MediaLibrary::openFolder(const QString &path) const {
    if (m_openUrl && QFileInfo(path).exists())
        m_openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void MediaLibrary::copyPath(const QString &path) const {
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(path);
}

} // namespace fugue
