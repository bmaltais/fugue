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
#include "engine/file_inputs.h"
#include "engine/file_roots.h"
#include "engine/ids.h"
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

// Whether this Fugue can open `document` as it was made (3.6), and the packs it needs: not
// installed, or installed at another major or an older minor. A built-in node this Fugue lacks,
// or has at such a version, means a newer Fugue made it.
bool opensHere(const Document &document, const Registry &registry, QStringList *neededPacks) {
    QSet<QString> packs;
    bool fits = true;
    const auto scan = [&](const Nodes &nodes) {
        for (const Node &node : nodes) {
            if (document.groups.contains(node.type))
                continue;
            const NodeType *type = registry.nodeTypeAt(node.type, node.version.major);
            const bool same = type && type->version.minor >= node.version.minor;
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
    *neededPacks = QStringList(packs.begin(), packs.end());
    neededPacks->sort();
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
                if (seen.contains(t.id))
                    continue; // the person's own, listed first, wins, even one that is broken
                seen.insert(t.id);
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
            t.models = document->models();
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
