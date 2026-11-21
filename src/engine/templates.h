#pragma once

// Workflow templates (spec 11.10): ready-to-run workflows the window lists and the CLI copies.
//
// A template is a folder:
//   <id>/template.json   {"title", "description", "category", "tags": [...]}; title required
//   <id>/workflow.fugue  the workflow; its `requires.models` give each model's `source`
//   <id>/preview.webp    (or .png, .jpg) what it makes; optional
//   <id>/inputs/         sample images its Load Image nodes name as `inputs/<name>`; optional
//   <id>/packs/<pack id>/ the exact pack its nodes need, a pack folder as 6.2 has it; optional.
//                        It is offered, never installed on its own: the person is asked and the
//                        pack trusted as for any pack (6.6, 6.7). The workflow's `requires.packs`
//                        stays the record of its version and where else to get it (4.3, #118).
// and a root of templates may hold a categories.json, {"categories": [{"id", "title"}]}, the
// order and titles of its categories. Nothing lists the templates: a root's folders are its
// templates, so there is no index to keep in step with them.
//
// What a template needs is read from its workflow, never written twice: the models it names
// (Document::models()) and the node versions it was made with. A template whose built-in
// nodes this Fugue lacks, or has at another major or an older minor (3.6), was made by a newer
// Fugue and is not listed; one whose pack is not installed, or installed at another version, is
// listed with the pack named.
//
// The bundled templates (the fugue-workflows repo, at the commit a Fugue release pins) and the
// person's own (~/Documents/Fugue/templates) are two roots of the same kind. The person's own
// folder may also hold plain .fugue files, each a template named after its file.

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/error.h"

namespace fugue {

class Document;
class Registry;

struct TemplateRoot {
    QString path;
    bool own = false; // the person's own templates (listed first, under "Your templates")
};

// A pack a template folder carries (`packs/<id>/`): its id and version as its pack.json says.
struct CarriedPack {
    QString id;
    QString version;
    QString folder; // absolute: what `fugue pack add` takes
};

struct Template {
    QString id;       // the folder's name (or a .fugue file's stem); a person's own wins a clash
    QString folder;   // absolute
    QString workflow; // absolute path of the workflow file
    QString preview;  // absolute path, empty when there is none
    QString title;
    QString description;
    QString category;      // id
    QString categoryTitle; // as the root's categories.json names it, else the id
    QStringList tags;
    bool own = false;
    QJsonArray models;         // Document::models(): resource, hash, name, source
    QStringList neededPacks;   // namespaces of its packs not installed, or installed at another version
    QList<CarriedPack> carriedPacks; // the packs the folder holds, whether or not installed here
};

// The namespaces (pack ids) of the node types `document` uses that are not installed at the
// major it recorded with at least its minor (3.6), built-in `fugue` excluded. What a window
// asks of a template's carried pack: still needed here, or installed now.
QStringList packsNeeded(const Document &document, const Registry &registry);

class TemplateLibrary {
public:
    TemplateLibrary(QList<TemplateRoot> roots, const Registry *registry);

    // The person's own folder, then the bundled one: $FUGUE_TEMPLATES when set; else
    // <prefix>/share/fugue/templates beside the running binary's bin/, else templates/ beside
    // the binary (the build folder links the source tree's there).
    static QList<TemplateRoot> standardRoots();
    static QString ownFolder(); // ~/Documents/Fugue/templates

    // Every template this Fugue can open: the person's own first, then by category in the
    // order categories.json gives (unnamed ones after, by id), then by title.
    QList<Template> list() const;
    // What kept a template out of list(), or is wrong with one in it, for the bundled
    // templates' check: an unreadable template.json or workflow, a missing title, a Load Image
    // file that is not in the folder. A template made by a newer Fugue is not a problem.
    Errors problems() const;

    // The template with this id, or nullptr.
    std::optional<Template> find(const QString &id) const;

    // Writes the template as a new workflow file at `path` (never replacing one), with its
    // sample inputs copied beside it under the names the workflow gives them (`inputs/<name>`).
    // A different file already at such a name stays: the sample takes `-2`, `-3`... and the
    // workflow names that instead. On a failure nothing it wrote is left.
    Errors copyTo(const Template &chosen, const QString &path) const;

private:
    void scan(QList<Template> *out, Errors *problems) const;

    QList<TemplateRoot> m_roots;
    const Registry *m_registry;
};

} // namespace fugue
