#include "apps/needs_model.h"

#include <algorithm>

#include "workflow/model_downloads.h"

namespace fugue {

namespace {

// A handle with this hash is anywhere inside `value` (derived handles nest theirs).
bool holdsHash(const QJsonValue &value, const QString &hash) {
    if (value.isArray()) {
        for (const QJsonValue item : value.toArray())
            if (holdsHash(item, hash))
                return true;
        return false;
    }
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    if (object.value("hash").toString() == hash)
        return true;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (holdsHash(it.value(), hash))
            return true;
    return false;
}

} // namespace

QString NeedsModel::nodeFor(const Document &document, const QVariantMap &item) {
    const QString kind = item.value("kind").toString();
    const QString hash = item.value("hash").toString();
    const QString pack = item.value("id").toString();
    for (auto node = document.graph.nodes.cbegin(); node != document.graph.nodes.cend(); ++node) {
        if (kind == QLatin1String("pack") && !pack.isEmpty() && node->type.section(QLatin1Char('/'), 0, 0) == pack)
            return node.key();
        if (kind == QLatin1String("model") && !hash.isEmpty())
            for (const InputValue &value : node->inputs)
                if (holdsHash(value.toJson(), hash))
                    return node.key();
    }
    return {};
}

NeedsModel::NeedsModel(QObject *parent) : QObject(parent) {
    m_soon.setSingleShot(true);
    m_soon.setInterval(40);
    connect(&m_soon, &QTimer::timeout, this, &NeedsModel::refresh);
}

void NeedsModel::refreshSoon() {
    if (!m_soon.isActive())
        m_soon.start();
}

void NeedsModel::setGraphAvailable(bool available) {
    const bool shown = available && bool(m_locate);
    if (m_graph == shown)
        return;
    m_graph = shown;
    emit changed();
}

void NeedsModel::refresh() {
    setPlan(m_source ? m_source() : ReadinessPlan());
}

void NeedsModel::setPlan(const ReadinessPlan &plan) {
    QVariantList items;
    QList<QJsonObject> entries;
    const auto add = [&](QVariantMap item, const QJsonObject &entry) {
        item.insert(QStringLiteral("node"), m_locate ? m_locate(item) : QString());
        items << item;
        entries << entry;
    };
    for (const ReadinessPack &row : plan.packs) {
        const PackSummary &p = row.pack;
        const QString name = p.name.isEmpty() ? p.id : p.name;
        QStringList parts{row.installs() ? tr("Install and trust") : tr("Trust")};
        if (row.hasEnvironment())
            parts << tr("sets up an environment");
        add({{"kind", "pack"}, {"key", "pack:" + p.id}, {"name", p.version.isEmpty() ? name : name + QLatin1Char(' ') + p.version},
             {"id", p.id}, {"hash", QString()}, {"detail", parts.join(QStringLiteral(" · "))},
             {"size", p.setupSize >= 0 && row.hasEnvironment() ? ModelDownloads::sizeText(p.setupSize) : QString()}, {"licence", QString()},
             {"licenceUrl", QString()}, {"canDownload", false}, {"action", tr("Install")}},
            {});
    }
    for (const ReadinessModel &m : plan.models) {
        QStringList parts;
        if (!m.host.isEmpty())
            parts << tr("from %1").arg(m.host);
        if (!m.onHost.isEmpty())
            parts << tr("on %1 if it has it").arg(m.onHost);
        if (m.deferred)
            parts << (m.typeFrom.isEmpty() ? tr("type from a pack in this list") : tr("type from %1").arg(m.typeFrom));
        add({{"kind", "model"}, {"key", "model:" + m.name}, {"name", m.name}, {"id", QString()}, {"hash", m.entry.value("hash").toString()},
             {"detail", parts.join(QStringLiteral(" · "))}, {"size", m.size >= 0 ? ModelDownloads::sizeText(m.size) : tr("size not recorded")},
             {"licence", m.license}, {"licenceUrl", m.licenseUrl}, {"canDownload", !m.deferred && !m.entry.isEmpty()},
             {"action", tr("Download")}},
            m.entry);
    }
    // What cannot be made ready from here has no action: a quiet line each.
    for (const Error &problem : plan.problems)
        add({{"kind", "problem"}, {"key", "problem:" + problem.message}, {"name", problem.message}, {"id", QString()}, {"hash", QString()},
             {"detail", problem.hint}, {"size", QString()}, {"licence", QString()}, {"licenceUrl", QString()}, {"canDownload", false},
             {"action", QString()}},
            {});

    QStringList counts;
    if (const int n = int(plan.packs.size()))
        counts << tr("%n packs", nullptr, n);
    if (const int n = int(plan.models.size()))
        counts << (plan.downloadBytes > 0 ? tr("%n models (%1)", nullptr, n).arg((plan.unknownSizes ? tr("at least ") : QString()) + ModelDownloads::sizeText(plan.downloadBytes))
                                          : tr("%n models", nullptr, n));
    const QString headline = counts.isEmpty() ? (items.isEmpty() ? QString() : tr("This app cannot be made ready from this machine."))
                                              : tr("This app needs %1.").arg(counts.join(tr(" and ")));
    const bool atOnce = plan.packs.isEmpty() && !plan.models.isEmpty() && !plan.lowSpace()
                        && std::none_of(plan.models.cbegin(), plan.models.cend(), [](const ReadinessModel &m) { return m.deferred; });

    if (items == m_items && headline == m_headline && atOnce == m_atOnce)
        return;
    m_items = items;
    m_entries = entries;
    m_headline = headline;
    m_atOnce = atOnce;
    emit changed();
}

void NeedsModel::download(int index) {
    if (index < 0 || index >= m_items.size())
        return;
    const QVariantMap item = m_items.at(index).toMap();
    if (item.value("kind").toString() == QLatin1String("problem"))
        return;
    if (item.value("canDownload").toBool())
        emit downloadRequested(m_entries.at(index));
    else
        emit getReadyRequested(false);
}

void NeedsModel::downloadAll() {
    if (m_items.isEmpty())
        return;
    emit getReadyRequested(m_atOnce);
}

void NeedsModel::showInGraph(int index) {
    if (!m_graph || index < 0 || index >= m_items.size())
        return;
    const QString node = m_items.at(index).toMap().value("node").toString();
    if (!node.isEmpty())
        emit showNodeRequested(node);
}

} // namespace fugue
