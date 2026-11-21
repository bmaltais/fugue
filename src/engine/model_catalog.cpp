#include "engine/model_catalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

#include "engine/file_hashes.h"
#include "engine/model_download.h"
#include "engine/model_library.h"
#include "engine/paths.h"

namespace fugue {

namespace {

// A relative file name that stays under its folder: no "..", no hidden part, no absolute path.
bool plainRelativePath(const QString &name) {
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    const QStringList parts = name.split(QLatin1Char('/'));
    return std::none_of(parts.begin(), parts.end(), [](const QString &part) {
        return part.isEmpty() || part.startsWith(QLatin1Char('.'));
    });
}

QString text(const QJsonObject &object, const char *key) { return object.value(QLatin1String(key)).toString().trimmed(); }

} // namespace

QJsonObject CatalogModel::handle() const {
    return QJsonObject{{"resource", resource}, {"hash", QStringLiteral("sha256:") + sha256}, {"name", file}};
}

QString CatalogModel::summary() const {
    QStringList parts;
    if (scale > 0)
        parts << QStringLiteral("%1x").arg(scale);
    if (!architecture.isEmpty())
        parts << architecture;
    if (!license.isEmpty())
        parts << license;
    parts << sizeText(size);
    return parts.join(QStringLiteral(" · "));
}

QString CatalogModel::keywords() const {
    return (QStringList{title, architecture, license, author, description} + tags).join(QLatin1Char(' '));
}

QString licenseLine(const CatalogModel &model) {
    QString line = model.license.isEmpty() ? QStringLiteral("license not stated") : model.license;
    if (!model.commercial)
        line += QStringLiteral(", not for commercial use");
    else if (model.attribution)
        line += QStringLiteral(", credit the author");
    return line;
}

ModelCatalog ModelCatalog::parse(const QByteArray &json, const QString &what) {
    ModelCatalog catalog;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() || document.object().value("catalog").toInt() != 1) {
        catalog.m_problems << QStringLiteral("%1 is not a model catalog (version 1).").arg(what);
        return catalog;
    }
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    for (const QJsonValue &value : document.object().value("models").toArray()) {
        const QJsonObject entry = value.toObject();
        CatalogModel model;
        model.id = text(entry, "id").toLower();
        const QString named = QStringLiteral("%1: %2").arg(what, model.id.isEmpty() ? QStringLiteral("a model with no id") : model.id);
        model.resource = text(entry, "resource");
        model.title = text(entry, "title");
        model.description = text(entry, "description");
        model.file = text(entry, "file");
        model.url = text(entry, "url");
        model.size = qint64(entry.value("size").toDouble());
        model.sha256 = text(entry, "sha256").toLower();
        if (model.sha256.startsWith(QLatin1String("sha256:")))
            model.sha256 = model.sha256.mid(7);
        model.scale = entry.value("scale").toInt();
        model.architecture = text(entry, "architecture");
        model.license = text(entry, "license");
        model.licenseUrl = text(entry, "license_url");
        model.commercial = entry.value("commercial").toBool(true);
        model.attribution = entry.value("attribution").toBool(false);
        model.author = text(entry, "author");
        model.source = text(entry, "source");
        for (const QJsonValue &tag : entry.value("tags").toArray())
            model.tags << tag.toString();
        QString why;
        if (model.id.isEmpty() || model.resource.isEmpty() || model.title.isEmpty())
            why = QStringLiteral("it needs an id, a resource and a title");
        else if (!plainRelativePath(model.file))
            why = QStringLiteral("its file name is not a plain relative path");
        else if (!isAllowedSource(QUrl(model.url)))
            why = QStringLiteral("its URL is not https");
        else if (!hex.match(model.sha256).hasMatch())
            why = QStringLiteral("its sha256 is not 64 hex digits");
        else if (model.size <= 0)
            why = QStringLiteral("its size is not given");
        else if (std::any_of(catalog.m_models.begin(), catalog.m_models.end(),
                             [&](const CatalogModel &other) { return other.id == model.id; }))
            why = QStringLiteral("its id is used twice");
        if (!why.isEmpty())
            catalog.m_problems << QStringLiteral("%1 is left out: %2.").arg(named, why);
        else
            catalog.m_models << model;
    }
    return catalog;
}

ModelCatalog ModelCatalog::load(const QString &userFolder) {
    ModelCatalog catalog;
    const auto add = [&](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            catalog.m_problems << QStringLiteral("%1 cannot be read.").arg(path);
            return;
        }
        catalog.merge(parse(file.readAll(), path));
    };
    for (const QString &name : QDir(QStringLiteral(":/fugue/catalog")).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
        add(QStringLiteral(":/fugue/catalog/") + name);
    const QDir user(userFolder.isEmpty() ? dataDir() + QStringLiteral("/catalog") : userFolder);
    for (const QString &name : user.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
        add(user.filePath(name));
    return catalog;
}

void ModelCatalog::merge(const ModelCatalog &other) {
    m_problems += other.m_problems;
    for (const CatalogModel &model : other.m_models) {
        const auto same = std::find_if(m_models.begin(), m_models.end(),
                                       [&](const CatalogModel &each) { return each.id == model.id; });
        if (same != m_models.end())
            *same = model;
        else
            m_models << model;
    }
}

QList<CatalogModel> ModelCatalog::forType(const QString &resource) const {
    QList<CatalogModel> out;
    for (const CatalogModel &model : m_models)
        if (model.resource == resource)
            out << model;
    return out;
}

const CatalogModel *ModelCatalog::find(const QString &id) const {
    for (const CatalogModel &model : m_models)
        if (model.id == id)
            return &model;
    return nullptr;
}

QList<CatalogModel> ModelCatalog::missing(const QString &resource, const ModelLibrary &library, const PortType &type,
                                          FileHashes &hashes) const {
    const QList<ModelFile> files = library.list(type);
    QList<CatalogModel> out;
    for (const CatalogModel &model : forType(resource)) {
        const bool here = std::any_of(files.begin(), files.end(), [&](const ModelFile &file) {
            return file.name == model.file || hashes.known(file.path) == QStringLiteral("sha256:") + model.sha256;
        });
        if (!here)
            out << model;
    }
    return out;
}

} // namespace fugue
