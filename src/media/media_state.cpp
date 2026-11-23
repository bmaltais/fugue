#include "media/media_state.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include "engine/json.h"

namespace fugue {

namespace {
const QStringList modes{"grid", "filmstrip", "single"};
const QStringList views{"files", "results"};
} // namespace

MediaState::MediaState(QString path) : m_path(std::move(path)) {
    if (m_path.isEmpty())
        return;
    const Result<QJsonObject> read = readJsonObject(m_path);
    if (!read)
        return;
    for (const QJsonValue &value : read->value("favorites").toArray())
        if (value.isString() && !value.toString().isEmpty())
            m_favorites.insert(value.toString());
    if (modes.contains(read->value("mode").toString()))
        m_mode = read->value("mode").toString();
    if (views.contains(read->value("view").toString()))
        m_view = read->value("view").toString();
    m_onlyFavorites = read->value("only_favorites").toBool();
}

bool MediaState::setFavorite(const QString &file, bool on) {
    if (file.isEmpty() || m_favorites.contains(file) == on)
        return false;
    if (on)
        m_favorites.insert(file);
    else
        m_favorites.remove(file);
    save();
    return true;
}

int MediaState::prune() {
    int dropped = 0;
    for (auto it = m_favorites.begin(); it != m_favorites.end();) {
        if (QFileInfo(*it).isFile()) {
            ++it;
        } else {
            it = m_favorites.erase(it);
            ++dropped;
        }
    }
    if (dropped > 0)
        save();
    return dropped;
}

bool MediaState::setMode(const QString &mode) {
    if (!modes.contains(mode) || mode == m_mode)
        return false;
    m_mode = mode;
    save();
    return true;
}

bool MediaState::setView(const QString &view) {
    if (!views.contains(view) || view == m_view)
        return false;
    m_view = view;
    save();
    return true;
}

bool MediaState::setOnlyFavorites(bool on) {
    if (on == m_onlyFavorites)
        return false;
    m_onlyFavorites = on;
    save();
    return true;
}

void MediaState::save() const {
    if (m_path.isEmpty())
        return;
    QStringList sorted(m_favorites.begin(), m_favorites.end());
    sorted.sort();
    writeJsonFile(m_path, QJsonObject{{"favorites", QJsonArray::fromStringList(sorted)},
                                      {"mode", m_mode},
                                      {"view", m_view},
                                      {"only_favorites", m_onlyFavorites}});
}

} // namespace fugue
