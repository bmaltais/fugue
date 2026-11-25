#include "engine/templates.h"

#include "engine/fugue_home.h"
#include "engine/paths.h"
#include "engine/project.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <functional>

#include "engine/app_binding.h"
#include "engine/atomic_file.h"
#include "engine/document.h"
#include "engine/file_inputs.h"
#include "engine/file_roots.h"
#include "engine/ids.h"
#include "engine/model_family.h"
#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue {

namespace {

const QString kAbout = QStringLiteral("template.json");
const QString kWorkflow = QStringLiteral("workflow.fugue");

// A template folder that is not as 11.10 says (no template.json, no title).
Error problem(const QString &message, const QString &hint = {}) {
    return makeError(QStringLiteral("invalid_document"), message, hint);
}

// `error` with the template it is about named first; its code kept.
Error named(const QString &id, Error error) {
    error.message = QStringLiteral("Template %1: %2").arg(id, error.message);
    return error;
}

// template.json's `source` (#369): the strings of name, author, url and note that are there. What is not
// as 11.10 says (not an object, a field that is not text, a url that is not http or https) is left out
// and said in `problems`, so a template with a bad link is still listed, with no link.
QJsonObject sourceOf(const QString &id, const QJsonObject &about, Errors *problems) {
    const QJsonValue raw = about.value(QStringLiteral("source"));
    if (raw.isUndefined())
        return {};
    const auto bad = [&](const QString &what) {
        if (problems)
            problems->append(problem(QStringLiteral("Template %1: %2 in %3.").arg(id, what, kAbout)));
    };
    if (!raw.isObject()) {
        bad(QStringLiteral("source must be an object with name, author, url and note"));
        return {};
    }
    QJsonObject out;
    const QJsonObject object = raw.toObject();
    for (const QString &key : {QStringLiteral("name"), QStringLiteral("author"), QStringLiteral("url"), QStringLiteral("note")}) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined())
            continue;
        const QString text = value.toString().trimmed();
        if (!value.isString()) {
            bad(QStringLiteral("source.%1 must be text").arg(key));
        } else if (key == QLatin1String("url") && !text.isEmpty()
                   && !text.startsWith(QLatin1String("https://")) && !text.startsWith(QLatin1String("http://"))) {
            bad(QStringLiteral("source.url must be an http or https address"));
        } else if (!text.isEmpty()) {
            out.insert(key, text);
        }
    }
    for (auto it = object.begin(); it != object.end(); ++it)
        if (it.key() != QLatin1String("name") && it.key() != QLatin1String("author")
            && it.key() != QLatin1String("url") && it.key() != QLatin1String("note"))
            bad(QStringLiteral("source has an unknown field %1").arg(it.key()));
    return out;
}

// template.json's `app` and the workflow's `app` block (#387): the card's words, or nothing when the template is not an app.
// The block is the binding and decides: a template is an app when its workflow has a block that reads, unless template.json
// says `false`. What does not agree is said in `problems` (the bundled templates' check), never kept out of the list: an
// `app` flag with no block, a block with no flag in a folder's template.json, a block that does not read (the template is
// then no app: it opens as a graph), a field that is not as 11.10 says.
std::optional<TemplateApp> appOf(const Template &t, const QJsonObject &about, const Document &document, bool hasAbout, Errors *problems) {
    const auto bad = [&](const QString &what) {
        if (problems)
            problems->append(problem(QStringLiteral("Template %1: %2.").arg(t.id, what)));
    };
    const QJsonValue raw = about.value(QStringLiteral("app"));
    if (raw.isBool() && !raw.toBool())
        return std::nullopt;
    const Result<std::optional<app::App>> block = app::appOf(document);
    if (!block) {
        bad(QStringLiteral("its app block does not read: %1").arg(block.error().message));
        return std::nullopt;
    }
    if (!*block) {
        if (!raw.isUndefined())
            bad(QStringLiteral("%1 says app, but the workflow has no app block").arg(kAbout));
        return std::nullopt;
    }
    if (hasAbout && raw.isUndefined())
        bad(QStringLiteral("the workflow has an app block, but %1 does not say \"app\"").arg(kAbout));
    QJsonObject card;
    if (raw.isObject()) {
        card = raw.toObject();
        for (auto it = card.begin(); it != card.end(); ++it) {
            const bool known = it.key() == QLatin1String("title") || it.key() == QLatin1String("summary") || it.key() == QLatin1String("kind");
            if (!known)
                bad(QStringLiteral("app has an unknown field %1 in %2").arg(it.key(), kAbout));
            else if (!it->isString())
                bad(QStringLiteral("app.%1 must be text in %2").arg(it.key(), kAbout));
        }
    } else if (!raw.isUndefined() && !raw.isBool()) {
        bad(QStringLiteral("app must be true or an object with title, summary and kind in %1").arg(kAbout));
    }
    const auto text = [&](const char *key, const QString &fallback) {
        const QString own = card.value(QLatin1String(key)).toString().trimmed();
        return own.isEmpty() ? fallback : own;
    };
    TemplateApp app;
    app.title = text("title", (*block)->title.trimmed()); // the block always has a title (4.9)
    app.summary = text("summary", (*block)->description.trimmed().isEmpty() ? t.description : (*block)->description.trimmed());
    app.kind = text("kind", QString());
    return app;
}

// A JSON object file, or why it is not one (13.3's codes: permission_denied, invalid_json,
// invalid_document).
Result<QJsonObject> readObject(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return makeError(QStringLiteral("permission_denied"),
                         QStringLiteral("%1 cannot be read: %2.").arg(path, file.errorString()));
    QJsonParseError parsed;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parsed);
    if (parsed.error != QJsonParseError::NoError)
        return makeError(QStringLiteral("invalid_json"),
                         QStringLiteral("%1 is not JSON: %2.").arg(path, parsed.errorString()));
    if (!json.isObject())
        return problem(QStringLiteral("%1 is not a JSON object.").arg(path));
    return json.object();
}

// Every `file` input literal of `document` (Load Image's picture), with where it is.
struct FileInput {
    QString node;
    QString text;
};
QList<FileInput> fileInputs(Document document, const Registry &registry) {
    QList<FileInput> out;
    forEachFileLiteral(document, registry, [&](const QString &node, const QString &, InputValue &value) {
        const QString text = value.literal().toString().trimmed();
        if (!text.isEmpty())
            out.append({node, text});
    });
    return out;
}

// The file a template's relative `text` names inside its `folder`, or empty when it names
// none there: an absolute or "~" path, or one that leads out, by `..` or by a link.
QString sampleIn(const QString &folder, const QString &text) {
    if (QDir::isAbsolutePath(text) || text.startsWith(QLatin1Char('~')))
        return {};
    const QString root = canonicalPath(folder);
    const QString path = canonicalPath(QDir(folder).filePath(text));
    return !root.isEmpty() && !path.isEmpty() && isInside(path, root) && QFileInfo(path).isFile() ? path : QString();
}

// The namespaces of the node types of `document` that are not installed at the major it
// recorded with at least its minor (3.6), `fugue` included.
QSet<QString> unmetNamespaces(const Document &document, const Registry &registry) {
    QSet<QString> unmet;
    const auto scan = [&](const Nodes &nodes) {
        for (const Node &node : nodes) {
            if (document.groups.contains(node.type))
                continue;
            const NodeType *type = registry.nodeTypeAt(node.type, node.version.major);
            if (!type || type->version.minor < node.version.minor)
                unmet.insert(typeNamespace(node.type));
        }
    };
    scan(document.graph.nodes);
    for (const GroupDefinition &group : document.groups)
        scan(group.nodes);
    return unmet;
}

QStringList sorted(QSet<QString> set) {
    QStringList list(set.begin(), set.end());
    list.sort();
    return list;
}

// Whether this Fugue can open `document` as it was made (3.6), and the packs it needs: not
// installed, or installed at another major or an older minor. A built-in node this Fugue lacks,
// or has at such a version, means a newer Fugue made it.
bool opensHere(const Document &document, const Registry &registry, QStringList *neededPacks) {
    QSet<QString> packs = unmetNamespaces(document, registry);
    const bool fits = !packs.remove(QStringLiteral("fugue"));
    *neededPacks = sorted(packs);
    return fits;
}

// The packs `<folder>/packs/<id>/` holds, by what each pack.json says. A folder that is not
// `<id>` (or has no readable id) is reported through `problems` as 11.10 says it must be.
QList<CarriedPack> carriedPacksOf(const QString &templateId, const QString &folder, const QJsonObject &recordedNeeds,
                                  Errors *problems) {
    QList<CarriedPack> out;
    const QDir packs(QDir(folder).filePath(QStringLiteral("packs")));
    for (const QFileInfo &entry : packs.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const Result<QJsonObject> read = readObject(QDir(entry.absoluteFilePath()).filePath(QStringLiteral("pack.json")));
        if (!read) {
            if (problems)
                problems->append(named(templateId, read.error()));
            continue;
        }
        const CarriedPack pack{read->value(QStringLiteral("id")).toString(), read->value(QStringLiteral("version")).toString(),
                               entry.absoluteFilePath(), read->value(QStringLiteral("name")).toString()};
        if (pack.id != entry.fileName() || pack.version.isEmpty()) {
            if (problems)
                problems->append(problem(QStringLiteral("Template %1: packs/%2 must hold the pack %2 with a version in its pack.json.")
                                             .arg(templateId, entry.fileName())));
            continue;
        }
        const QString recorded = recordedNeeds.value(QStringLiteral("packs")).toObject().value(pack.id).toObject()
                                     .value(QStringLiteral("version")).toString();
        if (problems && !recorded.isEmpty() && recorded != pack.version)
            problems->append(problem(QStringLiteral("Template %1: packs/%2 is version %3, but the workflow's requires.packs records %4.")
                                         .arg(templateId, pack.id, pack.version, recorded),
                                     QStringLiteral("Carry the pack version the workflow was made with.")));
        out.append(pack);
    }
    return out;
}

} // namespace

TemplatePreview previewFilesIn(const QString &folder) {
    const QDir dir(folder);
    // A plain file name of the folder that exists, else empty (no path parts: the key cannot reach outside the template).
    const auto file = [&dir](const QString &name) -> QString {
        if (name.isEmpty() || name != QFileInfo(name).fileName() || name.startsWith(QLatin1Char('.')))
            return {};
        const QString path = dir.filePath(name);
        return QFileInfo(path).isFile() ? path : QString();
    };
    const auto conventional = [&file] {
        for (const char *name : {"preview.webp", "preview.png", "preview.jpg"})
            if (const QString path = file(QLatin1String(name)); !path.isEmpty())
                return path;
        return QString();
    };
    QString named, poster;
    if (QFile about(dir.filePath(kAbout)); about.open(QIODevice::ReadOnly)) {
        const QJsonObject json = QJsonDocument::fromJson(about.readAll()).object();
        named = json.value(QStringLiteral("preview")).toString();
        poster = json.value(QStringLiteral("poster")).toString();
    }
    TemplatePreview out;
    if (named.endsWith(QLatin1String(".webm"), Qt::CaseInsensitive))
        out.video = file(named);
    else if (named.isEmpty())
        out.video = file(QStringLiteral("preview.webm"));
    else
        out.still = file(named);
    if (out.still.isEmpty() && !out.video.isEmpty())
        out.still = file(poster);
    if (out.still.isEmpty() && !out.video.isEmpty())
        out.still = file(QStringLiteral("poster.png"));
    if (out.still.isEmpty())
        out.still = conventional();
    return out;
}

TemplateLibrary::TemplateLibrary(QList<TemplateRoot> roots, const Registry *registry)
    : m_roots(std::move(roots)), m_registry(registry) {}

QString TemplateLibrary::ownFolder() {
    if (const QString inRoot = fugueHomeDir(QStringLiteral("documents")); !inRoot.isEmpty())
        return QDir::cleanPath(inRoot + QStringLiteral("/templates"));
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir::cleanPath((documents.isEmpty() ? QDir::homePath() : documents) + QStringLiteral("/Fugue/templates"));
}

QString TemplateLibrary::defaultOwnFolder() {
    if (fugueHome().isEmpty())
        return ownFolder();
    // The ordinary session's Documents folder: user-dirs.dirs of the base config folder (Qt reads the redirected
    // one inside a root), else ~/Documents.
    QString documents;
    QFile dirs(QFileInfo(baseConfigDir()).absolutePath() + QStringLiteral("/user-dirs.dirs"));
    if (dirs.open(QIODevice::ReadOnly))
        for (const QByteArray &line : dirs.readAll().split('\n')) {
            const QByteArray text = line.trimmed();
            if (text.startsWith("XDG_DOCUMENTS_DIR=")) {
                documents = QString::fromUtf8(text.mid(18)).remove(QLatin1Char('"'));
                documents.replace(QStringLiteral("$HOME"), QDir::homePath());
            }
        }
    if (documents.isEmpty() || !QDir::isAbsolutePath(documents))
        documents = QDir::homePath() + QStringLiteral("/Documents");
    return QDir::cleanPath(documents + QStringLiteral("/Fugue/templates"));
}

QList<TemplateRoot> TemplateLibrary::standardRoots() {
    QList<TemplateRoot> roots{{ownFolder(), true, false}};
    if (sessionInherits().contains(QStringLiteral("templates")))
        roots << TemplateRoot{defaultOwnFolder(), true, true};
    const QString given = qEnvironmentVariable("FUGUE_TEMPLATES");
    if (!given.isEmpty())
        return roots << TemplateRoot{QDir::cleanPath(given), false, false};
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString &candidate : {app + QStringLiteral("/../share/fugue/templates"), app + QStringLiteral("/templates")})
        if (QFileInfo(candidate).isDir())
            return roots << TemplateRoot{QDir::cleanPath(candidate), false, false};
    return roots;
}

void TemplateLibrary::scan(QList<Template> *out, Errors *problems) const {
    QSet<QString> seen;
    QList<std::pair<Template, int>> found; // with its category's place
    for (const TemplateRoot &root : m_roots) {
        const QDir dir(root.path);
        if (!dir.exists())
            continue;
        QHash<QString, std::pair<int, QString>> categories; // id -> place, title
        if (QFileInfo(dir.filePath(QStringLiteral("categories.json"))).isFile()) {
            const Result<QJsonObject> listed = readObject(dir.filePath(QStringLiteral("categories.json")));
            if (!listed && problems)
                problems->append(listed.errors());
            if (listed) {
                const QJsonArray array = listed->value(QStringLiteral("categories")).toArray();
                for (qsizetype i = 0; i < array.size(); ++i) {
                    const QJsonObject category = array.at(i).toObject();
                    categories.insert(category.value(QStringLiteral("id")).toString(),
                                      {int(i), category.value(QStringLiteral("title")).toString()});
                }
            }
        }

        for (const QFileInfo &entry : dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
            Template t;
            t.own = root.own;
            t.inherited = root.inherited;
            QJsonObject about;
            if (entry.isDir()) {
                const QDir folder(entry.absoluteFilePath());
                const bool hasAbout = QFileInfo(folder.filePath(kAbout)).isFile();
                if (!hasAbout && !QFileInfo(folder.filePath(kWorkflow)).isFile())
                    continue; // not a template folder
                t.id = entry.fileName();
                if (seen.contains(t.id))
                    continue; // the person's own, listed first, wins, even one that is broken
                seen.insert(t.id);
                t.folder = entry.absoluteFilePath();
                t.workflow = folder.filePath(kWorkflow);
                const TemplatePreview shown = previewFilesIn(t.folder);
                t.preview = shown.still;
                t.video = shown.video;
                if (!hasAbout) {
                    if (problems)
                        problems->append(problem(QStringLiteral("Template %1 has no %2.").arg(t.id, kAbout)));
                    continue;
                }
                const Result<QJsonObject> read = readObject(folder.filePath(kAbout));
                if (!read) {
                    if (problems)
                        problems->append(named(t.id, read.error()));
                    continue;
                }
                about = read.value();
                t.title = about.value(QStringLiteral("title")).toString().trimmed();
                if (t.title.isEmpty()) {
                    if (problems)
                        problems->append(problem(QStringLiteral("Template %1 has no title in its %2.").arg(t.id, kAbout)));
                    continue;
                }
            } else if (root.own && entry.suffix() == QLatin1String("fugue")) {
                t.id = entry.completeBaseName();
                if (seen.contains(t.id))
                    continue;
                seen.insert(t.id);
                t.folder = entry.absolutePath();
                t.workflow = entry.absoluteFilePath();
                t.title = t.id;
            } else {
                continue;
            }

            const Result<Document> document = Document::loadFile(t.workflow);
            if (!document) {
                if (problems)
                    problems->append(named(t.id, document.errors().first()));
                continue;
            }
            if (m_registry && !opensHere(*document, *m_registry, &t.neededPacks))
                continue;
            if (m_registry && problems)
                for (const FileInput &input : fileInputs(*document, *m_registry))
                    if (sampleIn(t.folder, input.text).isEmpty())
                        problems->append(makeError(QStringLiteral("file_not_found"),
                            QStringLiteral("Template %1: node %2 names %3, which is not a file in the template's folder.")
                                .arg(t.id, input.node, input.text),
                            QStringLiteral("Put the sample in the template's inputs folder and name it inputs/<name>.")));

            t.description = about.value(QStringLiteral("description")).toString();
            t.category = about.value(QStringLiteral("category")).toString();
            const auto category = categories.find(t.category);
            t.categoryTitle = category != categories.end() && !category->second.isEmpty() ? category->second : t.category;
            for (const QJsonValue tag : about.value(QStringLiteral("tags")).toArray())
                t.tags << tag.toString();
            t.source = sourceOf(t.id, about, problems);
            t.app = appOf(t, about, *document, entry.isDir(), problems);
            t.models = document->models();
            t.providerModels = document->providerModels();
            t.families = modelFamilies(t.models);
            if (entry.isDir()) // not a loose .fugue file: its folder is the person's own
                t.carriedPacks = carriedPacksOf(t.id, t.folder, document->requirements, problems);
            found.append({t, category != categories.end() ? category->first : int(categories.size())});
        }
    }
    if (!out)
        return;
    std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        if (a.first.own != b.first.own)
            return a.first.own;
        if (a.first.inherited != b.first.inherited)
            return b.first.inherited; // the session's own before the Default's
        if (a.second != b.second)
            return a.second < b.second;
        if (a.first.category != b.first.category)
            return a.first.category < b.first.category;
        return a.first.title.compare(b.first.title, Qt::CaseInsensitive) < 0;
    });
    for (const auto &entry : found)
        out->append(entry.first);
}

QStringList packsNeeded(const Document &document, const Registry &registry) {
    QSet<QString> packs = unmetNamespaces(document, registry);
    packs.remove(QStringLiteral("fugue"));
    return sorted(packs);
}

QString packTitle(const QString &id, const QString &name) {
    if (!name.trimmed().isEmpty())
        return name.trimmed();
    QString last = id.section(QLatin1Char('.'), -1);
    if (last.startsWith(QLatin1String("fugue-")) && last.size() > 6)
        last = last.mid(6);
    QStringList words = last.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    for (QString &word : words)
        word[0] = word.at(0).toUpper();
    return words.isEmpty() ? id : words.join(QLatin1Char(' '));
}

QList<Template> TemplateLibrary::list() const {
    QList<Template> out;
    scan(&out, nullptr);
    return out;
}

Errors TemplateLibrary::problems() const {
    Errors out;
    scan(nullptr, &out);
    return out;
}

std::optional<Template> TemplateLibrary::find(const QString &id) const {
    for (const Template &t : list())
        if (t.id == id)
            return t;
    return std::nullopt;
}

namespace {

Error saveFailed(const QString &what, const QString &why) {
    return makeError(QStringLiteral("permission_denied"), QStringLiteral("%1: %2.").arg(what, why));
}

// Copies the files and folders under `from` into `to`, never replacing a file.
Errors copyTree(const QString &from, const QString &to) {
    if (Errors made = makeFolder(to); !made.isEmpty())
        return made;
    for (const QFileInfo &entry : QDir(from).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name)) {
        const QString target = QDir(to).filePath(entry.fileName());
        if (entry.isSymLink())
            continue; // a link could point outside the template
        if (entry.isDir()) {
            if (Errors failed = copyTree(entry.absoluteFilePath(), target); !failed.isEmpty())
                return failed;
        } else if (!QFile::copy(entry.absoluteFilePath(), target)) {
            return {saveFailed(QStringLiteral("Cannot copy %1").arg(entry.absoluteFilePath()), QStringLiteral("it was not written to %1").arg(target))};
        }
    }
    return {};
}

} // namespace

Errors TemplateLibrary::saveOwn(const Template &chosen, const QByteArray &workflow) const {
    if (chosen.own && !chosen.inherited) {
        if (workflow.isEmpty())
            return {};
        return replaceFile(chosen.workflow, workflow, QFileInfo(chosen.workflow).fileName());
    }
    const QString target = QDir(ownFolder()).filePath(chosen.id);
    if (QFileInfo::exists(target))
        return {makeError(QStringLiteral("usage"), QStringLiteral("%1 already exists.").arg(target),
                          QStringLiteral("An own template of the same id would hide this one already; save over that."))};
    QByteArray bytes = workflow;
    if (bytes.isEmpty()) {
        QFile source(chosen.workflow);
        if (!source.open(QIODevice::ReadOnly))
            return {saveFailed(QStringLiteral("Cannot read %1").arg(chosen.workflow), source.errorString())};
        bytes = source.readAll();
    }
    const auto undo = [&target](Errors errors) {
        QDir(target).removeRecursively();
        return errors;
    };
    if (QFileInfo(chosen.workflow).fileName() == kWorkflow) {
        if (Errors failed = copyTree(chosen.folder, target); !failed.isEmpty())
            return undo(failed);
    } else { // a loose .fugue file of the Default's folder: a folder with a title of its own
        if (Errors made = makeFolder(target); !made.isEmpty())
            return undo(made);
        const QByteArray about = QJsonDocument(QJsonObject{{"title", chosen.title}}).toJson(QJsonDocument::Compact);
        if (Errors failed = replaceFile(QDir(target).filePath(kAbout), about, kAbout); !failed.isEmpty())
            return undo(failed);
    }
    if (Errors failed = replaceFile(QDir(target).filePath(kWorkflow), bytes, kWorkflow); !failed.isEmpty())
        return undo(failed);
    return {};
}

namespace {

// Sets the title the app card and the App view say: template.json's `title` and `app.title` (an `app: true` becomes an object),
// and the workflow's app block title. A template that is no app has no block to change.
Errors writeTitle(const QString &folder, const QString &title) {
    const auto edit = [](const QString &path, const std::function<void(QJsonObject &)> &change) -> Errors {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {saveFailed(QStringLiteral("Cannot read %1").arg(path), file.errorString())};
        QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        change(json);
        return replaceFile(path, QJsonDocument(json).toJson(QJsonDocument::Indented), QFileInfo(path).fileName());
    };
    const QString about = QDir(folder).filePath(kAbout);
    if (Errors failed = edit(about, [&title](QJsonObject &json) {
            json.insert(QStringLiteral("title"), title);
            if (json.value(QStringLiteral("app")).isBool() && json.value(QStringLiteral("app")).toBool())
                json.insert(QStringLiteral("app"), QJsonObject{{QStringLiteral("title"), title}});
            else if (json.value(QStringLiteral("app")).isObject()) {
                QJsonObject card = json.value(QStringLiteral("app")).toObject();
                card.insert(QStringLiteral("title"), title);
                json.insert(QStringLiteral("app"), card);
            }
        }); !failed.isEmpty())
        return failed;
    return edit(QDir(folder).filePath(kWorkflow), [&title](QJsonObject &json) {
        if (json.value(QStringLiteral("app")).isObject()) {
            QJsonObject block = json.value(QStringLiteral("app")).toObject();
            block.insert(QStringLiteral("title"), title);
            json.insert(QStringLiteral("app"), block);
        }
    });
}

} // namespace

Errors TemplateLibrary::duplicate(const Template &chosen, const QString &title, QString *newId) const {
    const QString name = title.trimmed();
    if (name.isEmpty())
        return {makeError(QStringLiteral("usage"), QStringLiteral("A copy needs a title."))};
    if (QFileInfo(chosen.workflow).fileName() != kWorkflow)
        return {makeError(QStringLiteral("usage"), QStringLiteral("%1 is a loose workflow file, not a template folder.").arg(chosen.workflow))};
    QString mine = ownFolder(); // the session's own folder: the first root that is its own and not inherited
    for (const TemplateRoot &root : m_roots)
        if (root.own && !root.inherited) {
            mine = root.path;
            break;
        }
    QString id = chosen.id + QStringLiteral("-copy");
    for (int n = 2; QFileInfo::exists(QDir(mine).filePath(id)) || find(id).has_value(); ++n)
        id = chosen.id + QStringLiteral("-copy-") + QString::number(n);
    const QString target = QDir(mine).filePath(id);
    const auto undo = [&target](Errors errors) {
        QDir(target).removeRecursively();
        return errors;
    };
    if (Errors failed = copyTree(chosen.folder, target); !failed.isEmpty())
        return undo(failed);
    if (Errors failed = writeTitle(target, name); !failed.isEmpty())
        return undo(failed);
    if (newId)
        *newId = id;
    return {};
}

Errors TemplateLibrary::rename(const Template &chosen, const QString &title) const {
    const QString name = title.trimmed();
    if (name.isEmpty())
        return {makeError(QStringLiteral("usage"), QStringLiteral("A template needs a title."))};
    if (!chosen.own || chosen.inherited || QFileInfo(chosen.workflow).fileName() != kWorkflow)
        return {makeError(QStringLiteral("usage"), QStringLiteral("%1 is not one of your own templates, so it cannot be renamed.").arg(chosen.id),
                          QStringLiteral("Make a copy and rename that."))};
    return writeTitle(chosen.folder, name);
}

Errors TemplateLibrary::copyTo(const Template &chosen, const QString &path) const {
    const Error exists = makeError(QStringLiteral("usage"), QStringLiteral("%1 already exists.").arg(path),
                                   QStringLiteral("Choose a new file name; fugue does not overwrite."));
    if (QFileInfo::exists(path))
        return {exists};
    QFile source(chosen.workflow);
    if (!source.open(QIODevice::ReadOnly))
        return {makeError(QStringLiteral("file_not_found"),
                          QStringLiteral("%1 cannot be read: %2.").arg(chosen.workflow, source.errorString()))};
    QByteArray workflow = source.readAll();
    Result<Document> document = Document::load(workflow);
    if (!document)
        return document.errors();

    // Where each sample goes, beside the new file under the name the workflow gives it; a
    // different file already there keeps its name, and the sample takes `-2`, `-3`... with the
    // workflow's literal changed to match. Planned before anything is written.
    const QDir to = QFileInfo(path).absoluteDir();
    QHash<QString, QString> renamed;              // literal -> new literal
    QList<std::pair<QString, QByteArray>> copies; // target, bytes
    QSet<QString> planned;
    for (const FileInput &input : m_registry ? fileInputs(*document, *m_registry) : QList<FileInput>()) {
        if (planned.contains(input.text))
            continue;
        planned.insert(input.text);
        QFile sample(sampleIn(chosen.folder, input.text));
        if (sample.fileName().isEmpty() || !sample.open(QIODevice::ReadOnly))
            continue; // a sample the template lacks: the node says so when it runs
        const QByteArray bytes = sample.readAll();
        const QFileInfo wanted(QDir::cleanPath(input.text));
        QString literal = wanted.filePath();
        for (int n = 2;; ++n) {
            QFile there(to.filePath(literal));
            if (!there.exists())
                break;
            if (there.open(QIODevice::ReadOnly) && there.readAll() == bytes)
                break; // a copy made before
            const QString suffix = wanted.suffix().isEmpty() ? QString() : QLatin1Char('.') + wanted.suffix();
            literal = QDir(wanted.path()).filePath(QStringLiteral("%1-%2%3").arg(wanted.completeBaseName()).arg(n).arg(suffix));
            literal = QDir::cleanPath(literal);
        }
        if (literal != input.text)
            renamed.insert(input.text, literal);
        if (!QFileInfo::exists(to.filePath(literal)))
            copies.append({to.filePath(literal), bytes});
    }
    if (!renamed.isEmpty()) {
        forEachFileLiteral(*document, *m_registry, [&](const QString &, const QString &, InputValue &value) {
            const QString text = value.literal().toString().trimmed();
            if (renamed.contains(text))
                value = InputValue::literal(renamed.value(text));
        });
        workflow = document->save();
    }

    // The samples, then the workflow; a failure removes the samples this wrote.
    QStringList written;
    const auto undo = [&written](Errors errors) {
        for (const QString &file : std::as_const(written))
            QFile::remove(file);
        return errors;
    };
    const auto write = [](const QString &target, const QByteArray &bytes) -> Result<bool> {
        const QString folder = QFileInfo(target).absolutePath();
        if (Errors made = makeFolder(folder); !made.isEmpty())
            return made;
        PartialFile partial(folder, QFileInfo(target).fileName());
        if (Errors failed = partial.write(bytes, target); !failed.isEmpty())
            return failed;
        return partial.commit(target, Overwrite::Never);
    };
    for (const auto &[target, bytes] : copies) {
        const Result<bool> done = write(target, bytes);
        if (!done)
            return undo(done.errors());
        if (done.value())
            written << target;
    }
    const Result<bool> done = write(path, workflow);
    if (!done)
        return undo(done.errors());
    if (!done.value())
        return undo({exists});
    return {};
}

} // namespace fugue
