#include "workflow/content_hash.h"

#include "engine/canonical.h"

#include <QCryptographicHash>
#include <QJsonDocument>

namespace fugue {

QString contentHash(const Document &document) {
    QJsonObject json = document.toJson();
    json.remove("requires");
    json.remove("run");
    if (json.contains("layout")) {
        QJsonObject layout = json.value("layout").toObject();
        layout.remove("view");
        json.insert("layout", layout);
    }
    const QString hash = canonicalHash(json);
    if (!hash.isEmpty())
        return hash;
    // Text with no canonical form (a lone surrogate) cannot be loaded, but a hand-built
    // document could hold it; its plain JSON still tells two contents apart.
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(json).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)
            .toHex());
}

} // namespace fugue
