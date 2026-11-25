#include "apps/app_shelf.h"

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
            {"ready", ready},
            {"needs", ready ? QString() : needs},
            {"detail", templ.value("status")},
            {"local", templ.value("local")},
            {"hosts", templ.value("hosts")},
            {"offlineHosts", templ.value("offlineHosts")}};
}

QVariantList AppShelf::cardsFrom(const QVariantList &templates) {
    QVariantList ready, waiting;
    for (const QVariant &entry : templates) {
        const QVariantMap card = cardFor(entry.toMap());
        if (card.isEmpty())
            continue;
        (card.value("ready").toBool() ? ready : waiting).append(card);
    }
    return ready + waiting;
}

void AppShelf::refresh() {
    // Told only when the cards differ: a list made again under a shelf being tapped would take the card from under the pointer.
    const QVariantList cards = cardsFrom(m_source ? m_source() : QVariantList());
    if (cards == m_apps)
        return;
    m_apps = cards;
    emit appsChanged();
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

void AppShelf::setLanding(bool landing) {
    if (landing == m_landing)
        return;
    const bool was = atHome();
    m_landing = landing;
    if (atHome() != was)
        emit homeChanged();
}

} // namespace fugue
