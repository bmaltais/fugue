#pragma once

// What the media browser keeps between sessions (#133), in one small file of the state folder
// ({"favorites": [paths], "mode": "grid", "view": "files", "only_favorites": false}): the files
// a person marked as favorites, the browser's mode (grid, filmstrip or single), which view the
// workflows panel was on (files or results) and whether only favorites are listed. Plain C++, no
// QML, tested alone. A file that is not there, or is not JSON, gives the defaults and is left as it is
// until something changes. A favorite is a path: it stays when the file is gone from the disk (a
// disk that comes back), and `prune` drops the ones that are not files any more.

#include <QSet>
#include <QString>

namespace fugue {

class MediaState {
public:
    // An empty path keeps nothing on disk (a test, or no state folder).
    explicit MediaState(QString path = {});

    bool isFavorite(const QString &file) const { return m_favorites.contains(file); }
    // True when it changed.
    bool setFavorite(const QString &file, bool on);
    int favoriteCount() const { return int(m_favorites.size()); }
    // Drops the favorites that are not files now; the count dropped.
    int prune();

    QString mode() const { return m_mode; }
    // "grid", "filmstrip" or "single"; anything else is ignored. True when it changed.
    bool setMode(const QString &mode);
    QString view() const { return m_view; }
    // "files" or "results"; anything else is ignored.
    bool setView(const QString &view);
    bool onlyFavorites() const { return m_onlyFavorites; }
    bool setOnlyFavorites(bool on);

private:
    void save() const;

    QString m_path;
    QSet<QString> m_favorites;
    QString m_mode = QStringLiteral("grid");
    QString m_view = QStringLiteral("files");
    bool m_onlyFavorites = false;
};

} // namespace fugue
