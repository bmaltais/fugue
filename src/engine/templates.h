#pragma once

// Workflow templates (spec 11.10): ready-to-run workflows the window lists and the CLI copies.
//
// A template is a folder:
//   <id>/template.json   {"title", "description", "category", "tags": [...], "source": {...}}; title required.
//                        `source` is optional, where the workflow comes from (#369): {"name", "author",
//                        "url", "note"}, each a string, all optional; a `url` must be http or https.
//                        `app` is optional, `true` or {"title", "summary", "kind"} (each text, all optional):
//                        the template is an App (#387, 4.9): its workflow has the `app` block, which is the
//                        binding; this is only the catalog's flag and the card's words (title and summary
//                        fall back to the block's (its title is required; the description, then the template's own); `kind` is a short word, as
//                        "image"). `false` keeps a workflow with a block out of the Apps.
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
// The bundled templates (the fugue-workflows repo, at the commit a Fugue release pins), the ones
// fetched since (engine/template_feed.h, #699) and the person's own (~/Documents/Fugue/templates)
// are roots of the same kind, searched in that order of precedence: own, fetched, bundled. The person's own
// folder may also hold plain .fugue files, each a template named after its file.

#include <QJsonArray>
#include <QJsonObject>
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
    // The Default project's own templates, which a project that inherits `templates` lists read-only (1.4.2),
    // after its own. `own` is set too: they are a person's, not Fugue's.
    bool inherited = false;
    // The templates fetched from fugue-workflows on their own (engine/template_feed.h, #699): listed after the person's own,
    // before the bundled ones. A template here that this Fugue cannot open does not hide the bundled one of the same id.
    // The fetched folder named by the store's `current` file when `path` is empty, read at each scan so no sha path is kept.
    bool online = false;
};

// A pack a template folder carries (`packs/<id>/`): its id and version as its pack.json says.
struct CarriedPack {
    QString id;
    QString version;
    QString folder; // absolute: what `fugue pack add` takes
    QString name;   // its pack.json's display name, empty when it has none
};

// What the catalog shows of an app template (#387): its card's words. Never empty except `kind`.
struct TemplateApp {
    QString title;
    QString summary;
    QString kind;
};

struct Template {
    QString id;       // the folder's name (or a .fugue file's stem); a person's own wins a clash
    QString folder;   // absolute
    QString workflow; // absolute path of the workflow file
    QString preview;  // absolute path of the still picture (the poster of a moving preview), empty when there is none
    QString video;    // absolute path of the moving preview (a WebM), empty when there is none (11.10)
    QString title;
    QString description;
    QString category;      // id
    QString categoryTitle; // as the root's categories.json names it, else the id
    QStringList tags;
    QJsonObject source;        // template.json's `source`: name, author, url, note (only the valid ones); empty when none
    std::optional<TemplateApp> app; // set when the workflow has a readable `app` block and template.json does not say `false`
    QStringList families;      // the model families of its models (engine/model_family.h), read, never written
    bool own = false;
    bool inherited = false;    // the Default project's own template, read-only here (1.4.2); `own` is set too
    bool online = false;       // from the fetched feed (#699), not bundled
    QJsonArray models;         // Document::models(): resource, hash, name, source
    QJsonArray providerModels; // Document::providerModels(): resource, name; a provider's models (no hash), as the fal.ai apps name
    QStringList neededPacks;   // namespaces of its packs not installed, or installed at another version
    QList<CarriedPack> carriedPacks; // the packs the folder holds, whether or not installed here
};

// The namespaces (pack ids) of the node types `document` uses that are not installed at the
// major it recorded with at least its minor (3.6), built-in `fugue` excluded. What a window
// asks of a template's carried pack: still needed here, or installed now.
QStringList packsNeeded(const Document &document, const Registry &registry);

// How a missing pack is named in words (#264): `name` when given (a pack.json's, or the `name` a
// workflow's requires.packs entry records), else the last part of the id, a leading "fugue-"
// dropped, hyphens as spaces, each word capitalized: "io.github.bmaltais.fugue-remove-background"
// is "Remove Background".
QString packTitle(const QString &id, const QString &name = {});

// What a template's folder shows of itself (spec 11.10): the still picture and the moving preview. template.json's `preview` names
// a file of the folder, a still (preview.webp, .png, .jpg) or a WebM; for a WebM, `poster` names its still, else poster.png, else
// the conventional preview.webp|png|jpg. Without the key the conventional stills are found, and a preview.webm beside them is the
// moving preview. A name that is not a plain file of the folder is ignored. Both absolute, empty when there is none.
struct TemplatePreview {
    QString still;
    QString video;
};
TemplatePreview previewFilesIn(const QString &folder);

class TemplateLibrary {
public:
    TemplateLibrary(QList<TemplateRoot> roots, const Registry *registry);

    // The person's own folder, then the fetched one, then the bundled one: $FUGUE_TEMPLATES when set (it replaces
    // both of the others); else the fetched folder (when settings.json's fetch_templates is on and one was
    // fetched), then <prefix>/share/fugue/templates beside the running binary's bin/, else templates/ beside
    // the binary (the build folder links the source tree's there).
    // A project that inherits `templates` (1.4.2) has the Default's own folder between them, tagged inherited.
    static QList<TemplateRoot> standardRoots();
    // The templates this release ships: <prefix>/share/fugue/templates beside the running binary's bin/, else templates/
    // beside the binary; empty when there is none.
    static QString bundledRoot();
    // The bundled folder is the build tree's `templates` link into the source checkout (a development build), not an install.
    static bool bundledIsBuildTree();
    static QString ownFolder(); // ~/Documents/Fugue/templates; a project's: <root>/documents/templates
    // The Default project's own folder (the ordinary session's ~/Documents/Fugue/templates), from inside a project too.
    static QString defaultOwnFolder();

    // Every template this Fugue can open: the person's own first, then by category in the
    // order categories.json gives (unnamed ones after, by id), then by title.
    QList<Template> list() const;
    // What kept a template out of list(), or is wrong with one in it, for the bundled
    // templates' check: an unreadable template.json or workflow, a missing title, a Load Image
    // file that is not in the folder, an `app` flag with no block in the workflow (or a block with no
    // flag in its template.json, or one that does not read). A template made by a newer Fugue is not a problem.
    Errors problems() const;

    // The template with this id, or nullptr.
    std::optional<Template> find(const QString &id) const;

    // Writes the template as a new workflow file at `path` (never replacing one), with its
    // sample inputs copied beside it under the names the workflow gives them (`inputs/<name>`).
    // A different file already at such a name stays: the sample takes `-2`, `-3`... and the
    // workflow names that instead. On a failure nothing it wrote is left.
    Errors copyTo(const Template &chosen, const QString &path) const;

    // Saves `workflow` (a workflow file's bytes; empty: the template's own) as this session's own template
    // `chosen.id`. Over one of the session's own, the file is replaced in place. Over a bundled one or one
    // inherited from the Default project (read-only), a copy is made in ownFolder(): its template.json,
    // preview, sample inputs and carried packs, and the new workflow; the copy hides the original (an own
    // template wins a clash, 11.10) and the original is never touched.
    Errors saveOwn(const Template &chosen, const QByteArray &workflow) const;

    // A copy of the template as a new own template under a new id (`<id>-copy`, `-copy-2`...) titled `title`: the whole folder
    // (workflow, preview, samples, carried packs) with the title written into template.json and the workflow's app block.
    // `newId` says the id on success. The original is never touched.
    Errors duplicate(const Template &chosen, const QString &title, QString *newId) const;
    // Renames one of the session's own templates (not a bundled or inherited one): template.json's title and app title, and the
    // workflow's app block title. The id (the folder) stays.
    Errors rename(const Template &chosen, const QString &title) const;

private:
    void scan(QList<Template> *out, Errors *problems) const;

    QList<TemplateRoot> m_roots;
    const Registry *m_registry;
};

} // namespace fugue
