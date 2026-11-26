#include "tour_probe.h"

#include <QDir>
#include <QFile>
#include <QQuickItem>
#include <QRegularExpression>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTimer>

namespace fugue {
namespace {

bool nameMatches(const QString &pattern, const QString &name) {
    return pattern.contains('*') ? QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern)).match(name).hasMatch() : name == pattern;
}

// Every visible item below `root` (root itself excluded) whose objectName matches.
void collect(QQuickItem *root, const QString &pattern, QList<QQuickItem *> &found) {
    for (QQuickItem *child : root->childItems()) {
        if (child->isVisible() && nameMatches(pattern, child->objectName()))
            found.append(child);
        collect(child, pattern, found);
    }
}

QList<QQuickItem *> resolve(QQuickWindow *window, const QStringList &steps) {
    QList<QQuickItem *> scope{window->contentItem()};
    for (const QString &step : steps) {
        QList<QQuickItem *> next;
        for (QQuickItem *item : scope)
            collect(item, step, next);
        scope = next;
    }
    return scope;
}

} // namespace

void startTourProbe(QQuickWindow *window, QObject *parent) {
    const QString folder = qEnvironmentVariable("FUGUE_TOUR_PROBE");
    if (folder.isEmpty() || !window)
        return;
    auto *timer = new QTimer(parent);
    timer->setInterval(150);
    QObject::connect(timer, &QTimer::timeout, parent, [window, askPath = QDir(folder).filePath("ask"), replyPath = QDir(folder).filePath("reply"), seen = QByteArray()]() mutable {
        QFile ask(askPath);
        if (!ask.open(QIODevice::ReadOnly))
            return;
        const QByteArray text = ask.readAll();
        if (text == seen)
            return;
        seen = text;
        QByteArray out;
        for (const QString &query : QString::fromUtf8(text).split('\n', Qt::SkipEmptyParts)) {
            if (query.startsWith('#')) // a line that only makes the file differ, so a repeated query is answered again
                continue;
            out += "? " + query.toUtf8() + '\n';
            for (QQuickItem *item : resolve(window, query.split('/', Qt::SkipEmptyParts))) {
                const QRectF box = item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); // a scaled view (after Fit) scales the size too
                out += item->objectName().toUtf8() + ' ' + QByteArray::number(qRound(box.x())) + ' ' + QByteArray::number(qRound(box.y())) + ' '
                       + QByteArray::number(qRound(box.width())) + ' ' + QByteArray::number(qRound(box.height())) + '\n';
            }
        }
        out += ".\n";
        QSaveFile reply(replyPath);
        if (reply.open(QIODevice::WriteOnly)) {
            reply.write(out);
            reply.commit();
        }
    });
    timer->start();
}

} // namespace fugue
