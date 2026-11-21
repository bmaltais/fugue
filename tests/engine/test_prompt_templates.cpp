// Prompt templates (spec 14.2, #140): what a template file may hold, how files merge, what the
// shipped ones are, and the system prompt a template and the node's options make.
// Seam: PromptTemplates over JSON text and real folders; systemPrompt() on a template.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/prompt_templates.h"

using namespace fugue;

namespace {

QJsonObject entry(const QString &id) {
    return QJsonObject{{"id", id}, {"label", "Label " + id}, {"family", "video"}, {"target", "Some model"},
                       {"system", "Write a prompt."}};
}

QByteArray fileOf(const QJsonArray &templates) {
    return QJsonDocument(QJsonObject{{"prompt_templates", 1}, {"templates", templates}}).toJson(QJsonDocument::Compact);
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

} // namespace

class TestPromptTemplates : public QObject {
    Q_OBJECT

private slots:
    void aValidTemplateIsKept() {
        const PromptTemplates templates = PromptTemplates::parse(fileOf({entry("one")}), "t");
        QVERIFY(templates.problems().isEmpty());
        QCOMPARE(templates.ids(), QStringList{"one"});
        QCOMPARE(templates.find("one")->label, QString("Label one"));
        QCOMPARE(templates.find("one")->family, QString("video"));
        QVERIFY(!templates.find("two"));
    }
    void aSystemPromptMayBeAListOfLines() {
        QJsonObject lines = entry("one");
        lines["system"] = QJsonArray{"First line.", "Second line."};
        const PromptTemplates templates = PromptTemplates::parse(fileOf({lines}), "t");
        QCOMPARE(templates.find("one")->system, QString("First line.\nSecond line."));
    }
    void aTemplateThatIsNotValidIsLeftOutAndSaid() {
        QJsonObject noLabel = entry("a");
        noLabel.remove("label");
        QJsonObject noSystem = entry("b");
        noSystem.remove("system");
        QJsonObject badFamily = entry("c");
        badFamily["family"] = "text";
        QJsonObject longSystem = entry("d");
        longSystem["system"] = QString(20001, 'x');
        QJsonObject badExample = entry("e");
        badExample["examples"] = QJsonArray{QJsonObject{{"idea", "a fox"}}};
        QJsonObject noSeconds = entry("f");
        noSeconds["duration_note"] = "It is long.";
        QJsonObject noLanguage = entry("g");
        noLanguage["language_note"] = "Write it well.";
        const PromptTemplates templates = PromptTemplates::parse(
            fileOf({entry("Bad Id"), noLabel, noSystem, badFamily, longSystem, badExample, noSeconds, noLanguage,
                    entry("ok"), entry("ok")}),
            "mine.json");
        QCOMPARE(templates.ids(), QStringList{"ok"});
        QCOMPARE(templates.problems().size(), 9);
        QVERIFY2(templates.problems().at(0).contains("mine.json: Bad Id is left out: its id"),
                 qPrintable(templates.problems().at(0)));
        QVERIFY(templates.problems().at(1).contains("no label"));
        QVERIFY(templates.problems().at(2).contains("no system prompt"));
        QVERIFY(templates.problems().at(3).contains("family"));
        QVERIFY(templates.problems().at(4).contains("longer than"));
        QVERIFY(templates.problems().at(5).contains("example"));
        QVERIFY(templates.problems().at(6).contains("{seconds}"));
        QVERIFY(templates.problems().at(7).contains("{language}"));
        QVERIFY(templates.problems().at(8).contains("used twice"));
    }
    void aFileThatIsNotATemplateFileIsSaid() {
        QCOMPARE(PromptTemplates::parse("[]", "x.json").problems().size(), 1);
        QCOMPARE(PromptTemplates::parse("{\"catalog\": 1}", "x.json").problems().size(), 1);
    }
    void aLaterFileReplacesATemplateOfTheSameIdInPlace() {
        PromptTemplates first = PromptTemplates::parse(fileOf({entry("a"), entry("b")}), "1");
        QJsonObject changed = entry("a");
        changed["label"] = "Changed";
        first.merge(PromptTemplates::parse(fileOf({changed, entry("c")}), "2"));
        QCOMPARE(first.ids(), (QStringList{"a", "b", "c"}));
        QCOMPARE(first.find("a")->label, QString("Changed"));
    }
    void theShippedTemplatesAreValidAndKeepTheOriginalStyles() {
        const QTemporaryDir none;
        const PromptTemplates templates = PromptTemplates::load(none.filePath("nothing-here"));
        QVERIFY2(templates.problems().isEmpty(), qPrintable(templates.problems().join("\n")));
        // The first three are Enhance Prompt's original styles, in their order, then the new ones.
        QCOMPARE(templates.ids().mid(0, 3), (QStringList{"detailed", "photograph", "tags"}));
        for (const QString id : {"video", "minimax-video", "video-from-image", "music"})
            QVERIFY2(templates.find(id), qPrintable(id));
        for (const PromptTemplate &each : templates.all()) {
            QVERIFY2(each.system.contains("alone"), qPrintable(each.id + " must ask for the prompt alone"));
            QVERIFY2(!each.target.isEmpty() && !each.notes.isEmpty(), qPrintable(each.id));
            for (const PromptExample &example : each.examples)
                QVERIFY(!example.idea.isEmpty() && !example.prompt.isEmpty());
        }
        QCOMPARE(templates.find("music")->family, QString("audio"));
        QCOMPARE(templates.find("minimax-video")->family, QString("video"));
    }
    void aFileInTheUsersFolderAddsAndReplaces() {
        QTemporaryDir dir;
        QJsonObject mine = entry("my-style");
        mine["family"] = "image";
        QJsonObject over = entry("tags");
        over["system"] = "My tags. Answer with the tags alone.";
        over["family"] = "image";
        writeFile(dir.filePath("mine.json"), fileOf({mine, over}));
        writeFile(dir.filePath("broken.json"), "{");
        writeFile(dir.filePath("notes.txt"), "ignored");
        const PromptTemplates templates = PromptTemplates::load(dir.path());
        QVERIFY(templates.find("my-style"));
        QCOMPARE(templates.find("tags")->system, QString("My tags. Answer with the tags alone."));
        QCOMPARE(templates.ids().mid(0, 3), (QStringList{"detailed", "photograph", "tags"})); // replaced in place
        QCOMPARE(templates.problems().size(), 1);
        QVERIFY(templates.problems().first().contains("broken.json"));
    }
    void theSystemPromptIsTheTemplateAloneWhenNoOptionIsGiven() {
        const PromptTemplate prompt = PromptTemplates::parse(fileOf({entry("a")}), "t").all().first();
        QCOMPARE(systemPrompt(prompt, {}), QString("Write a prompt."));
    }
    void examplesFollowTheInstructionsAndOptionsComeAfterThem() {
        QJsonObject full = entry("a");
        full["examples"] = QJsonArray{QJsonObject{{"idea", "a fox"}, {"prompt", "A red fox runs."}}};
        full["duration_note"] = "It lasts {seconds} s.";
        const PromptTemplate prompt = PromptTemplates::parse(fileOf({full}), "t").all().first();
        PromptOptions options;
        options.language = "français";
        options.durationSeconds = 8;
        options.instructions = " At night. ";
        QCOMPARE(systemPrompt(prompt, options),
                 QString("Write a prompt.\n\nExamples:\n\nIdea: a fox\nPrompt: A red fox runs."
                         "\n\nWrite the prompt in français.\n\nIt lasts 8 s.\n\nAlso: At night."));
    }
    void aTemplateWithoutALengthNoteIgnoresTheLength() {
        const PromptTemplate prompt = PromptTemplates::parse(fileOf({entry("a")}), "t").all().first();
        PromptOptions options;
        options.durationSeconds = 8;
        QCOMPARE(systemPrompt(prompt, options), QString("Write a prompt."));
    }
    void aTemplatesOwnLanguageNoteReplacesTheGeneralSentence() {
        QJsonObject own = entry("a");
        own["language_note"] = "Use {language} but keep quotes.";
        const PromptTemplate prompt = PromptTemplates::parse(fileOf({own}), "t").all().first();
        PromptOptions options;
        options.language = "Spanish";
        QVERIFY(systemPrompt(prompt, options).endsWith("Use Spanish but keep quotes."));
    }
    void theStyleSummaryListsEachTemplate() {
        QJsonObject own = entry("a");
        own["notes"] = "Use it well.";
        const PromptTemplates templates = PromptTemplates::parse(fileOf({own, entry("b")}), "t");
        QCOMPARE(styleSummary(templates), QString("a: Label a, for Some model. Use it well.\nb: Label b, for Some model"));
    }
};

QTEST_GUILESS_MAIN(TestPromptTemplates)
#include "test_prompt_templates.moc"
