#include "engine/templates.h"

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

#include "engine/atomic_file.h"
#include "engine/document.h"
#include "engine/ids.h"
#include "engine/node_type.h"
#include "engine/registry.h"

namespace fugue {

namespace {

const QString kAbout = QStringLiteral("template.json");
const QString kWorkflow = QStringLiteral("workflow.fugue");

Error problem(const QString &message, const QString &hint = {}) {
    return makeError(QStringLiteral("invalid_document"), message, hint);
}

// A JSON object file, or why it is not one.
Result<QJsonObject> readObject(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return problem(QStringLiteral("%1 cannot be read: %2.").arg(path, file.errorString()));
    QJsonParseError parsed;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parsed);
    if (parsed.error != QJsonParseError::NoError || !json.isObject())
        return problem(QStringLiteral("%1 is not a JSON object: %2.")
                           .arg(path, parsed.error != QJsonParseError::NoError ? parsed.errorString()
                                                                               : QStringLiteral("not an object")));
    return json.object();
}

// Every `file` input literal of `document` (Load Image's picture), with where it is.
struct FileInput {
    QString node;
    QString text;
};
QList<FileInput> fileInputs(const Document &document, const Registry &registry) {
    QList<FileInput> out;
    const auto scan = [&](const Nodes &nodes) {
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            const NodeType *type = registry.nodeType(it->type);
            if (!type)
                continue;
            for (auto in = it->inputs.begin(); in != it->inputs.end(); ++in) {
                const InputSpec *spec = type->input(in.key());
                if (spec && spec->widget() == QLatin1String("file") && in->kind() == InputValue::Kind::Literal &&
                    in->literal().isString() && !in->literal().toString().trimmed().isEmpty())
                    out.append({it.key(), in->literal().toString().trimmed()});
            }
        }
    };
    scan(document.graph.nodes);
    for (const GroupDefinition &group : document.groups)
        scan(group.nodes);
    return out;
}

// A relative path that stays inside its folder ("inputs/cat.png", not "../x" or "/x").
bool staysInside(const QString &text) {
    const QString clean = QDir::cleanPath(text);
    return !QDir::isAbsolutePath(text) && !text.startsWith(QLatin1Char('~')) && clean != QLatin1String("..") &&
           !clean.startsWith(QLatin1String("../"));
}

// Whether this Fugue can open `document` as it was made (3.6), and the packs it lacks. A
// built-in node this Fugue lacks, or has at another major or an older minor, means a newer
// Fugue made it.
bool opensHere(const Document &document, const Registry &registry, QStringList *missingPacks) {
    QSet<QString> packs;
    bool fits = true;
    const auto scan = [&](const Nodes &nodes) {
        for (const Node &node : nodes) {
            if (document.groups.contains(node.type))
                continue;
            const NodeType *type = registry.nodeType(node.type);
            const bool same = type && type->version.major == node.version.major && type->version.minor >= node.version.minor;
            if (same)
                continue;
            const QString pack = typeNamespace(node.type);
            if (pack == QLatin1String("fugue"))
                fits = false;
            else
                packs.insert(pack);
        }
    };
    scan(document.graph.nodes);
    for (const GroupDefinition &group : document.groups)
        scan(group.nodes);
    *missingPacks = QStringList(packs.begin(), packs.end());
    missingPacks->sort();
    return fits;
}

QString previewOf(const QString &folder) {
    for (const char *name : {"preview.webp", "preview.png", "preview.jpg"})
        if (QFileInfo(QDir(folder).filePath(QLatin1String(name))).isFile())
            return QDir(folder).filePath(QLatin1String(name));
    return {};
}

} // namespace

TemplateLibrary::TemplateLibrary(QList<TemplateRoot> roots, const Registry *registry)
    : m_roots(std::move(roots)), m_registry(registry) {}

QString TemplateLibrary::ownFolder() {
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir::cleanPath((documents.isEmpty() ? QDir::homePath() : documents) + QStringLiteral("/Fugue/templates"));
}

QList<TemplateRoot> TemplateLibrary::standardRoots() {
    QList<TemplateRoot> roots{{ownFolder(), true}};
    const QString given = qEnvironmentVariable("FUGUE_TEMPLATES");
    if (!given.isEmpty())
        return roots << TemplateRoot{QDir::cleanPath(given), false};
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString &candidate : {app + QStringLiteral("/../share/fugue/templates"), app + QStringLiteral("/templates")})
        if (QFileInfo(candidate).isDir())
            return roots << TemplateRoot{QDir::cleanPath(candidate), false};
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
            QJsonObject about;
            if (entry.isDir()) {
                const QDir folder(entry.absoluteFilePath());
                const bool hasAbout = QFileInfo(folder.filePath(kAbout)).isFile();
                if (!hasAbout && !QFileInfo(folder.filePath(kWorkflow)).isFile())
                    continue; // not a template folder
                t.id = entry.fileName();
                t.folder = entry.absoluteFilePath();
                t.workflow = folder.filePath(kWorkflow);
                t.preview = previewOf(t.folder);
                if (!hasAbout) {
                    if (problems)
                        problems->append(problem(QStringLiteral("Template %1 has no %2.").arg(t.id, kAbout)));
                    continue;
                }
                const Result<QJsonObject> read = readObject(folder.filePath(kAbout));
                if (!read) {
                    if (problems)
                        problems->append(problem(QStringLiteral("Template %1: %2").arg(t.id, read.error().message)));
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
                t.folder = entry.absolutePath();
                t.workflow = entry.absoluteFilePath();
                t.title = t.id;
            } else {
                continue;
            }
            if (seen.contains(t.id))
                continue; // the person's own, listed first, wins

            const Result<Document> document = Document::loadFile(t.workflow);
            if (!document) {
                if (problems)
                    problems->append(problem(QStringLiteral("Template %1: %2").arg(t.id, document.errors().first().message)));
                continue;
            }
            if (m_registry && !opensHere(*document, *m_registry, &t.missingPacks))
                continue;
            if (m_registry && problems)
                for (const FileInput &input : fileInputs(*document, *m_registry))
                    if (!staysInside(input.text) || !QFileInfo(QDir(t.folder).filePath(input.text)).isFile())
                        problems->append(problem(
                            QStringLiteral("Template %1: node %2 names %3, which is not a file in the template's folder.")
                                .arg(t.id, input.node, input.text),
                            QStringLiteral("Put the sample in the template's inputs folder and name it inputs/<name>.")));

            t.description = about.value(QStringLiteral("description")).toString();
            t.category = about.value(QStringLiteral("category")).toString();
            const auto category = categories.find(t.category);
            t.categoryTitle = category != categories.end() && !category->second.isEmpty() ? category->second : t.category;
            for (const QJsonValue tag : about.value(QStringLiteral("tags")).toArray())
                t.tags << tag.toString();
            t.models = document->models();
            seen.insert(t.id);
            found.append({t, category != categories.end() ? category->first : int(categories.size())});
        }
    }
    if (!out)
        return;
    std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        if (a.first.own != b.first.own)
            return a.first.own;
        if (a.second != b.second)
            return a.second < b.second;
        if (a.first.category != b.first.category)
            return a.first.category < b.first.category;
        return a.first.title.compare(b.first.title, Qt::CaseInsensitive) < 0;
    });
    for (const auto &entry : found)
        out->append(entry.first);
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

Errors TemplateLibrary::copyTo(const Template &chosen, const QString &path) const {
    if (QFileInfo::exists(path))
        return {makeError(QStringLiteral("usage"), QStringLiteral("%1 already exists.").arg(path),
                          QStringLiteral("Choose a new file name; fugue does not overwrite."))};
    QFile source(chosen.workflow);
    if (!source.open(QIODevice::ReadOnly))
        return {makeError(QStringLiteral("file_not_found"),
                          QStringLiteral("%1 cannot be read: %2.").arg(chosen.workflow, source.errorString()))};
    const QByteArray workflow = source.readAll();
    const Result<Document> document = Document::load(workflow);
    if (!document)
        return document.errors();

    // The samples to copy, each checked before anything is written.
    const QDir to = QFileInfo(path).absoluteDir();
    QList<std::pair<QString, QByteArray>> copies; // target, bytes
    QSet<QString> planned;
    for (const FileInput &input : m_registry ? fileInputs(*document, *m_registry) : QList<FileInput>()) {
        if (!staysInside(input.text) || planned.contains(input.text))
            continue;
        QFile sample(QDir(chosen.folder).filePath(input.text));
        if (!sample.open(QIODevice::ReadOnly))
            continue; // a sample the template lacks: the node says so when it runs
        const QByteArray bytes = sample.readAll();
        const QString target = to.filePath(input.text);
        QFile there(target);
        if (there.exists()) {
            if (there.open(QIODevice::ReadOnly) && there.readAll() == bytes)
                continue; // a copy made before
            return {makeError(QStringLiteral("usage"),
                              QStringLiteral("%1 is already there with other content.").arg(target),
                              QStringLiteral("Write the workflow into another folder."))};
        }
        planned.insert(input.text);
        copies.append({target, bytes});
    }
    for (const auto &[target, bytes] : copies) {
        const QString folder = QFileInfo(target).absolutePath();
        if (Errors made = makeFolder(folder); !made.isEmpty())
            return made;
        PartialFile partial(folder, QFileInfo(target).fileName());
        if (Errors written = partial.write(bytes, target); !written.isEmpty())
            return written;
        const Result<bool> committed = partial.commit(target, Overwrite::Never);
        if (!committed)
            return committed.errors();
    }
    PartialFile partial(to.absolutePath(), QFileInfo(path).fileName());
    if (Errors written = partial.write(workflow, path); !written.isEmpty())
        return written;
    const Result<bool> committed = partial.commit(path, Overwrite::Never);
    if (!committed)
        return committed.errors();
    if (!committed.value())
        return {makeError(QStringLiteral("usage"), QStringLiteral("%1 already exists.").arg(path),
                          QStringLiteral("Choose a new file name; fugue does not overwrite."))};
    return {};
}

} // namespace fugue
