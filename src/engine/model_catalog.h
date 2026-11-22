#pragma once

// The model catalog (spec 8.9, #134): models a person can download, curated and shipped with
// Fugue, for any resource type. A catalog file lists, for each model, what is needed to fetch it
// with the download machinery (engine/model_download.h) and to decide whether to: the file's name,
// an https URL (http only to this machine, as downloads take it), its size and sha256 (a file is kept only when both match, so a wrong source can
// do no harm), and what a person should read first: the license, whether it allows commercial use,
// who made it, where it is hosted.
//
// Catalog files are JSON, {"catalog": 1, "models": [{...}]}; see src/catalog/upscale-models.json.
// Fugue's own are compiled in (`:/fugue/catalog/*.json`); the person's, in dataDir()/catalog/*.json,
// are read after them (and after those of installed packs: <pack folder>/catalog/*.json under the
// pack search paths, engine/pack_loader.h), and an entry there with the id of an earlier one replaces it, so a catalog
// is updated by dropping a newer file in (no code or release needed). An entry that is not valid
// (no https URL, a sha256 that is not 64 hex digits, a name that is not a relative file name, no
// size, a repeated id) is left out and said in problems(): a model whose hash was not checked has
// no place in a catalog.
//
// Nothing here fetches anything. Fetching is a person's decision (spec 6.7: nothing silent), made
// where the model is picked, with the license shown (canvas/widget_edit.h).

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace fugue {

class FileHashes;
class ModelLibrary;
struct PortType;

struct CatalogModel {
    QString id;           // stable, lower case: replaced by a later file's entry of the same id
    QString resource;     // the resource type it is a file of ("fugue/upscale-model")
    QString title;        // "Real-ESRGAN x4plus"
    QString description;  // one short line: what it is good for
    QString file;         // the name it is saved under (a relative path under the type's folder)
    QString url;          // https
    qint64 size = 0;      // in bytes
    QString sha256;       // bare lower-case hex
    int scale = 0;        // for an upscaler: 2, 4, ...; 0 when it has none
    QString architecture; // "ESRGAN (RRDBNet)"
    QString license;      // an SPDX id when there is one
    QString licenseUrl;
    bool commercial = true;   // false when the license forbids commercial use
    bool attribution = false; // true when the license asks for credit
    QString author;
    QString source;       // the page it is hosted on, for a person to read
    QStringList tags;

    // The handle that names it once it is on disk (2.5): {resource, hash: "sha256:…", name}.
    QJsonObject handle() const;
    // "4x · ESRGAN (RRDBNet) · BSD-3-Clause · 64 MB": what a row shows and a filter matches.
    QString summary() const;
    // Every word a filter matches: the title, the architecture, the license, the author, the tags.
    QString keywords() const;
};

class ModelCatalog {
public:
    // Fugue's own catalogs, then the files of `userFolder` (default dataDir()/catalog); both are
    // read each time, so a file dropped in shows at once.
    // With no `userFolder` the packs of packSearchPaths() are read; with one (tests), the folders of
    // `packPaths`, none by default.
    static ModelCatalog load(const QString &userFolder = {}, const QStringList &packPaths = {});
    // One catalog's text, as a file holds it (tests, and load()); what it says wrong goes to problems().
    static ModelCatalog parse(const QByteArray &json, const QString &what);

    // The entries, in the order the files list them, for one resource type.
    QList<CatalogModel> forType(const QString &resource) const;
    const CatalogModel *find(const QString &id) const;
    const QList<CatalogModel> &all() const { return m_models; }
    // What was left out, one sentence each, naming the file and the model.
    const QStringList &problems() const { return m_problems; }

    // Adds `other`'s entries: one with the id of an entry here replaces it, in its place.
    void merge(const ModelCatalog &other);

    // The entries of `resource` not on disk yet: no file in the type's folders is named as the
    // entry is or has its sha256 remembered (engine/file_hashes). Reads no model file.
    QList<CatalogModel> missing(const QString &resource, const ModelLibrary &library, const PortType &type,
                                FileHashes &hashes) const;

private:
    QList<CatalogModel> m_models;
    QStringList m_problems;
};

// The license line a confirmation shows: "BSD-3-Clause", "CC-BY-4.0, credit the author", or
// "CC-BY-NC-SA-4.0, not for commercial use". Not translated: it is the license's own wording.
QString licenseLine(const CatalogModel &model);

} // namespace fugue
