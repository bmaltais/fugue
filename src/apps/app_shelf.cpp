#include "apps/app_shelf.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

#include "engine/atomic_file.h"

namespace fugue {

namespace {

// What the sheet will ask for, in few words: "Needs 2.1 GB and 1 pack". The size says the models when it is known.
QString needsText(const QVariantMap &templ) {
    const int models = int(templ.value("missingModels").toList().size());
    const int packs = int(templ.value("neededPacks").toStringList().size());
    const QString size = templ.value("downloadSize").toString();
    const QString modelPart = models == 0 ? QString() : !size.isEmpty() ? size : AppShelf::tr("%n models", nullptr, models);
    const QString packPart = packs == 0 ? QString() : AppShelf::tr("%n packs", nullptr, packs);
    if (!modelPart.isEmpty() && !packPart.isEmpty())
        return AppShelf::tr("Needs %1 and %2").arg(modelPart, packPart);
    if (!modelPart.isEmpty() || !packPart.isEmpty())
        return AppShelf::tr("Needs %1").arg(modelPart.isEmpty() ? packPart : modelPart);
    return {};
}

} // namespace

AppShelf::AppShelf(Source source, Open open, QObject *parent)
    : QObject(parent), m_source(std::move(source)), m_open(std::move(open)) {}

QVariantMap AppShelf::cardFor(const QVariantMap &templ) {
    if (!templ.value("app").toBool())
        return {};
    // Models on a paired host count as here (`ready`, #164); a pack that is not installed does not.
    const QString needs = needsText(templ);
    const bool ready = templ.value("ready").toBool() && templ.value("neededPacks").toStringList().isEmpty();
    return {{"id", templ.value("id")},
            {"title", templ.value("title")},
            {"summary", templ.value("description")},
            {"preview", templ.value("preview")},
            {"video", templ.value("video")},
            {"kind", templ.value("appKind")},
            {"own", templ.value("own")},
            {"inherited", templ.value("inherited")},
            {"canRename", templ.value("own").toBool() && !templ.value("inherited").toBool()},
            {"bookmarked", false},
            {"ready", ready},
            {"needs", ready ? QString() : needs},
            {"detail", templ.value("status")},
            {"local", templ.value("local")},
            {"hosts", templ.value("hosts")},
            {"offlineHosts", templ.value("offlineHosts")}};
}

QVariantList AppShelf::cardsFrom(const QVariantList &templates, const QStringList &bookmarks) {
    QVariantList marked, ready, waiting;
    for (const QVariant &entry : templates) {
        QVariantMap card = cardFor(entry.toMap());
        if (card.isEmpty())
            continue;
        const bool pinned = bookmarks.contains(card.value("id").toString());
        card.insert("bookmarked", pinned);
        (pinned ? marked : card.value("ready").toBool() ? ready : waiting).append(card);
    }
    return marked + ready + waiting;
}

bool AppShelf::matches(const QString &query, const QVariantMap &card) {
    const QStringList words = query.split(QChar(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return true;
    const QString text = card.value("title").toString() + QChar(' ') + card.value("summary").toString() + QChar(' ')
                         + card.value("kind").toString();
    return std::all_of(words.cbegin(), words.cend(), [&text](const QString &word) { return text.contains(word, Qt::CaseInsensitive); });
}

void AppShelf::rebuild(const QVariantList &templates) {
    m_templates = templates;
    const QVariantList cards = cardsFrom(templates, m_bookmarks);
    if (cards != m_apps) {
        m_apps = cards;
        emit appsChanged();
    }
    narrow();
}

void AppShelf::narrow() {
    QVariantList shown;
    for (const QVariant &card : m_apps)
        if (matches(m_query, card.toMap()))
            shown.append(card);
    if (shown == m_shown)
        return;
    m_shown = shown;
    emit shownChanged();
}

void AppShelf::setQuery(const QString &query) {
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    narrow();
}

void AppShelf::setBookmarksFile(const QString &path) {
    m_bookmarksFile = path;
    m_bookmarks.clear();
    if (QFile file(path); file.open(QIODevice::ReadOnly))
        for (const QJsonValue id : QJsonDocument::fromJson(file.readAll()).object().value("bookmarks").toArray())
            if (!id.toString().isEmpty() && !m_bookmarks.contains(id.toString()))
                m_bookmarks << id.toString();
    rebuild(m_templates);
}

void AppShelf::saveBookmarks() const {
    if (m_bookmarksFile.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_bookmarksFile).absolutePath());
    const QByteArray bytes = QJsonDocument(QJsonObject{{"bookmarks", QJsonArray::fromStringList(m_bookmarks)}}).toJson();
    replaceFile(m_bookmarksFile, bytes, QFileInfo(m_bookmarksFile).fileName()); // a failure keeps them for the session
}

void AppShelf::toggleBookmark(const QString &id) {
    if (!m_bookmarks.removeOne(id))
        m_bookmarks << id;
    saveBookmarks();
    rebuild(m_templates);
}

void AppShelf::setActions(Duplicate duplicate, Rename rename) {
    m_duplicate = std::move(duplicate);
    m_rename = std::move(rename);
}

bool AppShelf::duplicate(const QString &id) {
    const auto card = std::find_if(m_apps.cbegin(), m_apps.cend(), [&id](const QVariant &c) { return c.toMap().value("id") == id; });
    if (!m_duplicate || card == m_apps.cend())
        return false;
    const QString made = m_duplicate(id, tr("%1 copy").arg(card->toMap().value("title").toString()));
    if (made.isEmpty())
        return false;
    refresh();
    return choose(made);
}

bool AppShelf::rename(const QString &id, const QString &title) {
    const auto card = std::find_if(m_apps.cbegin(), m_apps.cend(), [&id](const QVariant &c) { return c.toMap().value("id") == id; });
    if (!m_rename || card == m_apps.cend() || !card->toMap().value("canRename").toBool() || title.trimmed().isEmpty())
        return false;
    if (!m_rename(id, title.trimmed()))
        return false;
    refresh();
    return true;
}

void AppShelf::refresh() {
    // Told only when the cards differ: a list made again under a shelf being tapped would take the card from under the pointer.
    rebuild(m_source ? m_source() : QVariantList());
}

bool AppShelf::choose(const QString &id) {
    if (!m_open || !m_open(id))
        return false;
    emit chosen(id);
    return true;
}

void AppShelf::dismiss() {
    if (m_dismissed)
        return;
    const bool was = atHome();
    m_dismissed = true;
    if (was)
        emit homeChanged();
}

void AppShelf::showAgain() {
    if (!m_dismissed)
        return;
    m_dismissed = false;
    if (m_landing)
        emit homeChanged();
}

void AppShelf::setLanding(bool landing) {
    if (landing == m_landing)
        return;
    m_landing = landing;
    emit homeChanged(); // atHome and landing both read it
}

} // namespace fugue
