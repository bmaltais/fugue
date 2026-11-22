#include "engine/prompt_templates.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

#include "engine/paths.h"
#include "engine/project.h"

namespace fugue {

namespace {

// A string, or a list of strings joined with newlines (a long prompt reads better as lines).
QString textOf(const QJsonValue &value) {
    if (value.isArray()) {
        QStringList lines;
        for (const QJsonValue &line : value.toArray())
            lines << line.toString();
        return lines.join(QLatin1Char('\n')).trimmed();
    }
    return value.toString().trimmed();
}

constexpr int kLongest = 20000; // characters of one template's instructions: a model's prompt, not a book

} // namespace

PromptTemplates PromptTemplates::parse(const QByteArray &json, const QString &what) {
    PromptTemplates templates;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value("prompt_templates").toInt() != 1) {
        templates.m_problems << QStringLiteral("%1 is not a prompt template file (version 1).").arg(what);
        return templates;
    }
    static const QRegularExpression idPattern(QStringLiteral("^[a-z][a-z0-9]*(-[a-z0-9]+)*$"));
    for (const QJsonValue &value : document.object().value("templates").toArray()) {
        const QJsonObject entry = value.toObject();
        PromptTemplate prompt;
        prompt.id = textOf(entry.value("id"));
        const QString named = QStringLiteral("%1: %2").arg(
            what, prompt.id.isEmpty() ? QStringLiteral("a template with no id") : prompt.id);
        prompt.label = textOf(entry.value("label"));
        prompt.family = textOf(entry.value("family"));
        prompt.target = textOf(entry.value("target"));
        prompt.system = textOf(entry.value("system"));
        prompt.notes = textOf(entry.value("notes"));
        prompt.durationNote = textOf(entry.value("duration_note"));
        prompt.languageNote = textOf(entry.value("language_note"));
        bool examplesOk = true;
        for (const QJsonValue &each : entry.value("examples").toArray()) {
            const QJsonObject example = each.toObject();
            const PromptExample made{textOf(example.value("idea")), textOf(example.value("prompt"))};
            examplesOk = examplesOk && !made.idea.isEmpty() && !made.prompt.isEmpty();
            prompt.examples << made;
        }
        QString why;
        if (!idPattern.match(prompt.id).hasMatch())
            why = QStringLiteral("its id is not lower case words joined by hyphens");
        else if (prompt.label.isEmpty())
            why = QStringLiteral("it has no label");
        else if (!QStringList{"image", "video", "audio", "describe"}.contains(prompt.family))
            why = QStringLiteral("its family is not image, video, audio or describe");
        else if (prompt.system.isEmpty())
            why = QStringLiteral("it has no system prompt");
        else if (prompt.system.size() > kLongest)
            why = QStringLiteral("its system prompt is longer than %1 characters").arg(kLongest);
        else if (!examplesOk)
            why = QStringLiteral("an example has no idea or no prompt");
        else if (!prompt.durationNote.isEmpty() && !prompt.durationNote.contains(QLatin1String("{seconds}")))
            why = QStringLiteral("its duration_note does not say {seconds}");
        else if (!prompt.languageNote.isEmpty() && !prompt.languageNote.contains(QLatin1String("{language}")))
            why = QStringLiteral("its language_note does not say {language}");
        else if (templates.find(prompt.id))
            why = QStringLiteral("its id is used twice");
        if (!why.isEmpty())
            templates.m_problems << QStringLiteral("%1 is left out: %2.").arg(named, why);
        else
            templates.m_templates << prompt;
    }
    return templates;
}

PromptTemplates PromptTemplates::load(const QString &userFolder, const QString &inheritedFolder) {
    PromptTemplates templates;
    const auto add = [&](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            templates.m_problems << QStringLiteral("%1 cannot be read.").arg(path);
            return;
        }
        templates.merge(parse(file.readAll(), path));
    };
    for (const QString &name : QDir(QStringLiteral(":/fugue/prompts")).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
        add(QStringLiteral(":/fugue/prompts/") + name);
    QString fromDefault = inheritedFolder;
    if (userFolder.isEmpty() && sessionInherits().contains(QStringLiteral("prompts")))
        fromDefault = baseDataDir() + QStringLiteral("/prompts");
    if (!fromDefault.isEmpty() && QDir::cleanPath(fromDefault) != QDir::cleanPath(userFolder.isEmpty() ? dataDir() + QStringLiteral("/prompts") : userFolder)) {
        const QDir inherited(fromDefault);
        for (const QString &name : inherited.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QFile file(inherited.filePath(name));
            if (!file.open(QIODevice::ReadOnly)) {
                templates.m_problems << QStringLiteral("%1 cannot be read.").arg(file.fileName());
                continue;
            }
            PromptTemplates theirs = parse(file.readAll(), file.fileName());
            for (PromptTemplate &prompt : theirs.m_templates)
                prompt.inherited = true;
            templates.merge(theirs);
        }
    }
    const QDir user(userFolder.isEmpty() ? dataDir() + QStringLiteral("/prompts") : userFolder);
    for (const QString &name : user.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
        add(user.filePath(name));
    // Image, video, then audio, each in the order its files list them: the dropdown reads by what a
    // prompt is for, and "detailed" stays first.
    const QStringList families{"image", "video", "audio", "describe"};
    std::stable_sort(templates.m_templates.begin(), templates.m_templates.end(),
                     [&](const PromptTemplate &a, const PromptTemplate &b) {
                         return families.indexOf(a.family) < families.indexOf(b.family);
                     });
    return templates;
}

void PromptTemplates::merge(const PromptTemplates &other) {
    m_problems += other.m_problems;
    for (const PromptTemplate &prompt : other.m_templates) {
        const auto same = std::find_if(m_templates.begin(), m_templates.end(),
                                       [&](const PromptTemplate &each) { return each.id == prompt.id; });
        if (same != m_templates.end())
            *same = prompt;
        else
            m_templates << prompt;
    }
}

const PromptTemplate *PromptTemplates::find(const QString &id) const {
    for (const PromptTemplate &prompt : m_templates)
        if (prompt.id == id)
            return &prompt;
    return nullptr;
}

QStringList PromptTemplates::ids() const {
    QStringList ids;
    for (const PromptTemplate &prompt : m_templates)
        ids << prompt.id;
    return ids;
}

QStringList PromptTemplates::ids(const QStringList &families) const {
    QStringList ids;
    for (const PromptTemplate &prompt : m_templates)
        if (families.contains(prompt.family))
            ids << prompt.id;
    return ids;
}

QStringList promptFamilies() { return {QStringLiteral("image"), QStringLiteral("video"), QStringLiteral("audio")}; }

QString systemPrompt(const PromptTemplate &prompt, const PromptOptions &options) {
    QString system = prompt.system;
    if (!prompt.examples.isEmpty()) {
        system += QStringLiteral("\n\nExamples:");
        for (const PromptExample &example : prompt.examples)
            system += QStringLiteral("\n\nIdea: %1\nPrompt: %2").arg(example.idea, example.prompt);
    }
    const QString language = options.language.trimmed();
    if (!language.isEmpty()) {
        QString note = prompt.languageNote.isEmpty() ? QStringLiteral("Write the prompt in {language}.") : prompt.languageNote;
        system += QStringLiteral("\n\n") + note.replace(QLatin1String("{language}"), language);
    }
    if (options.durationSeconds > 0 && !prompt.durationNote.isEmpty()) {
        QString note = prompt.durationNote;
        system += QStringLiteral("\n\n") + note.replace(QLatin1String("{seconds}"), QString::number(options.durationSeconds));
    }
    const QString extra = options.instructions.trimmed();
    if (!extra.isEmpty())
        system += QStringLiteral("\n\nAlso: ") + extra;
    return system;
}

QString styleSummary(const PromptTemplates &templates, const QStringList &families) {
    QStringList lines;
    for (const PromptTemplate &prompt : templates.all()) {
        if (!families.isEmpty() && !families.contains(prompt.family))
            continue;
        QString line = prompt.id + QStringLiteral(": ") + prompt.label;
        if (!prompt.target.isEmpty())
            line += QStringLiteral(", for ") + prompt.target;
        if (!prompt.notes.isEmpty())
            line += QStringLiteral(". ") + prompt.notes;
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace fugue
