// `fugue skill install` and `uninstall` (spec 13.5) as the real binary, with a home of their own:
// no test touches the owner's ~/.claude or ~/.agents. And the bundled skill itself, which must
// stay in step with the command table, the error codes of 13.3 and a workflow that runs.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include "cli/args.h"

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QString firstCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
    QStringList actions() const {
        QStringList list;
        for (const QJsonValue action : data().value("actions").toArray())
            list << action.toObject().value("target").toString() + ":" + action.toObject().value("action").toString();
        return list;
    }
};

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}

QString sourceSkill() { return QStringLiteral(FUGUE_SOURCE_DIR "/share/skills/fugue/SKILL.md"); }

} // namespace

class TestCliSkill : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    QString home() const { return m_dir.filePath("home"); }
    QString agents() const { return home() + "/.agents/skills/fugue"; }
    QString claude() const { return home() + "/.claude/skills/fugue"; }

    Outcome fugue(const QStringList &args, const QString &cwd = QString(), const QString &claudeConfig = QString()) const {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("CLAUDE_CONFIG_DIR");
        if (!claudeConfig.isEmpty())
            env.insert("CLAUDE_CONFIG_DIR", claudeConfig);
        env.insert("XDG_DATA_HOME", m_dir.filePath("data"));
        env.insert("XDG_STATE_HOME", m_dir.filePath("state"));
        env.insert("XDG_CACHE_HOME", m_dir.filePath("cache"));
        env.insert("XDG_CONFIG_HOME", m_dir.filePath("config"));
        env.insert("HOME", home());
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(cwd.isEmpty() ? m_dir.path() : cwd);
        process.start(FUGUE_BINARY, QStringList(args) << "--json");
        process.closeWriteChannel();
        Outcome outcome;
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            return outcome;
        }
        outcome.exitCode = process.exitCode();
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    void clean() {
        QDir(home()).removeRecursively();
        QDir().mkpath(home());
    }

private slots:
    void init() { clean(); }

    void dryRunWritesNothingAndSaysWhere() {
        const Outcome out = fugue({"skill", "install", "--dry-run"});
        QCOMPARE(out.exitCode, 0);
        QVERIFY(out.data().value("dry_run").toBool());
        QCOMPARE(out.actions(), (QStringList{"agents:create", "claude:link"}));
        QVERIFY(out.out.contains(agents().toUtf8()));
        QVERIFY(out.out.contains(claude().toUtf8()));
        QVERIFY(!QFileInfo::exists(home() + "/.agents"));
        QVERIFY(!QFileInfo::exists(home() + "/.claude"));
    }

    void installPutsOneCopyInAgentsAndLinksClaudeToIt() {
        const Outcome out = fugue({"skill", "install"});
        QCOMPARE(out.exitCode, 0);
        QCOMPARE(out.actions(), (QStringList{"agents:create", "claude:link"}));
        QCOMPARE(readFile(agents() + "/SKILL.md"), readFile(sourceSkill())); // the binary carries the source
        QVERIFY(QFileInfo(claude()).isSymLink());
        QCOMPARE(QFileInfo(claude()).symLinkTarget(), agents());
        QCOMPARE(readFile(claude() + "/SKILL.md"), readFile(sourceSkill()));
    }

    void installAgainChangesNothing() {
        QCOMPARE(fugue({"skill", "install"}).exitCode, 0);
        const Outcome again = fugue({"skill", "install"});
        QCOMPARE(again.exitCode, 0);
        QCOMPARE(again.actions(), (QStringList{"agents:unchanged", "claude:unchanged"}));
    }

    void anOlderUnmodifiedCopyIsUpdated() {
        // What an earlier Fugue installed: a body of its own and the manifest that records its hash.
        const QByteArray old = "---\nname: fugue\ndescription: old\n---\nold body\n";
        writeFile(agents() + "/SKILL.md", old);
        writeFile(agents() + "/.fugue-skill.json",
                  QJsonDocument(QJsonObject{{"name", "fugue"},
                                            {"sha256", QString::fromLatin1(QCryptographicHash::hash(old, QCryptographicHash::Sha256).toHex())}})
                      .toJson());
        const Outcome out = fugue({"skill", "install"});
        QCOMPARE(out.exitCode, 0);
        QCOMPARE(out.actions(), (QStringList{"agents:update", "claude:link"}));
        QCOMPARE(readFile(agents() + "/SKILL.md"), readFile(sourceSkill()));
    }

    void aModifiedFileIsNeverOverwrittenWithoutForce() {
        QCOMPARE(fugue({"skill", "install"}).exitCode, 0);
        writeFile(agents() + "/SKILL.md", "my own notes\n");
        const Outcome refused = fugue({"skill", "install"});
        QCOMPARE(refused.exitCode, 1);
        QCOMPARE(refused.firstCode(), QString("skill_modified"));
        QVERIFY(refused.out.contains("--force"));
        QCOMPARE(readFile(agents() + "/SKILL.md"), QByteArray("my own notes\n"));
        // A dry run reports the same and still writes nothing.
        QCOMPARE(fugue({"skill", "install", "--dry-run"}).firstCode(), QString("skill_modified"));
        QCOMPARE(readFile(agents() + "/SKILL.md"), QByteArray("my own notes\n"));

        const Outcome forced = fugue({"skill", "install", "--force"});
        QCOMPARE(forced.exitCode, 0);
        QCOMPARE(readFile(agents() + "/SKILL.md"), readFile(sourceSkill()));
        // The user's file is kept beside it.
        QStringList backups = QDir(QFileInfo(agents()).absolutePath()).entryList({"fugue.bak-*"}, QDir::Dirs);
        QCOMPARE(backups.size(), 1);
        QCOMPARE(readFile(QFileInfo(agents()).absolutePath() + "/" + backups.first() + "/SKILL.md"), QByteArray("my own notes\n"));
    }

    void anotherToolsClaudeEntryIsLeftAlone() {
        writeFile(claude() + "/SKILL.md", "someone else's fugue skill\n");
        const Outcome refused = fugue({"skill", "install"});
        QCOMPARE(refused.exitCode, 1);
        QCOMPARE(refused.firstCode(), QString("skill_modified"));
        QVERIFY(!QFileInfo::exists(agents())); // a plan that fails writes nothing
        QCOMPARE(readFile(claude() + "/SKILL.md"), QByteArray("someone else's fugue skill\n"));
    }

    void targetsChooseWhereItGoes() {
        QCOMPARE(fugue({"skill", "install", "--target", "agents"}).actions(), (QStringList{"agents:create"}));
        QVERIFY(!QFileInfo::exists(home() + "/.claude"));
        clean();
        // Alone, Claude gets a copy of its own, not a link to something that is not there.
        const Outcome claudeOnly = fugue({"skill", "install", "--target=claude"});
        QCOMPARE(claudeOnly.actions(), (QStringList{"claude:create"}));
        QVERIFY(!QFileInfo(claude()).isSymLink());
        QCOMPARE(readFile(claude() + "/SKILL.md"), readFile(sourceSkill()));
        QVERIFY(!QFileInfo::exists(home() + "/.agents"));
        // Then all: the copy becomes a link to the shared one.
        QCOMPARE(fugue({"skill", "install"}).actions(), (QStringList{"agents:create", "claude:link"}));
        QVERIFY(QFileInfo(claude()).isSymLink());

        const Outcome bad = fugue({"skill", "install", "--target", "emacs"});
        QCOMPARE(bad.exitCode, 1);
        QCOMPARE(bad.firstCode(), QString("usage"));
        QVERIFY(bad.out.contains("agents") && bad.out.contains("claude") && bad.out.contains("all"));
    }

    void claudeConfigDirMovesClaudesFolder() {
        const QString config = m_dir.filePath("cfg");
        const Outcome out = fugue({"skill", "install"}, QString(), config);
        QCOMPARE(out.exitCode, 0);
        QVERIFY(QFileInfo(config + "/skills/fugue").isSymLink());
        QVERIFY(!QFileInfo::exists(home() + "/.claude"));
    }

    void uninstallRemovesWhatItInstalled() {
        QCOMPARE(fugue({"skill", "install"}).exitCode, 0);
        const Outcome dry = fugue({"skill", "uninstall", "--dry-run"});
        QCOMPARE(dry.actions(), (QStringList{"claude:remove", "agents:remove"}));
        QVERIFY(QFileInfo(agents() + "/SKILL.md").isFile());
        const Outcome out = fugue({"skill", "uninstall"});
        QCOMPARE(out.exitCode, 0);
        QVERIFY(!QFileInfo(claude()).isSymLink() && !QFileInfo::exists(claude()));
        QVERIFY(!QFileInfo::exists(agents()));
        QCOMPARE(fugue({"skill", "uninstall"}).actions(), (QStringList{"claude:absent", "agents:absent"}));
    }

    void uninstallKeepsAModifiedFile() {
        QCOMPARE(fugue({"skill", "install"}).exitCode, 0);
        writeFile(agents() + "/SKILL.md", "edited\n");
        const Outcome refused = fugue({"skill", "uninstall"});
        QCOMPARE(refused.exitCode, 1);
        QCOMPARE(refused.firstCode(), QString("skill_modified"));
        QCOMPARE(readFile(agents() + "/SKILL.md"), QByteArray("edited\n"));
        QVERIFY(QFileInfo(claude()).isSymLink());
    }

    // The bundled skill is for agents, so it is held to what Fugue really is.

    void skillHasNameAndDescription() {
        const QString text = QString::fromUtf8(readFile(sourceSkill()));
        QVERIFY(text.startsWith("---\nname: fugue\ndescription:"));
        QVERIFY(text.indexOf("\n---\n", 4) > 0);
    }

    void skillNamesEveryCommandAndOption() {
        const QString text = QString::fromUtf8(readFile(sourceSkill()));
        QStringList missing;
        for (const fugue::cli::CommandInfo &info : fugue::cli::implementedCommands()) {
            const QString words = "fugue " + QString(info.name).replace('.', ' ');
            if (!text.contains(words))
                missing << words;
            for (const QString &option : info.valueOptions + info.flagOptions)
                if (!text.contains("--" + option))
                    missing << words + " --" + option;
        }
        QVERIFY2(missing.isEmpty(), qPrintable("not in the skill: " + missing.join("; ")));
    }

    void skillListsEveryErrorCodeOfTheSpec() {
        const QString spec = QString::fromUtf8(readFile(QStringLiteral(FUGUE_SOURCE_DIR "/docs/spec.md")));
        const int from = spec.indexOf("Codes in v0.1:");
        const int to = spec.indexOf("New codes may be added", from);
        QVERIFY(from > 0 && to > from);
        const QString skill = QString::fromUtf8(readFile(sourceSkill()));
        QStringList codes;
        QRegularExpressionMatchIterator it = QRegularExpression("`([a-z]+(?:_[a-z]+)+)`").globalMatch(spec.mid(from, to - from));
        while (it.hasNext())
            codes << it.next().captured(1);
        QVERIFY(codes.size() > 40);
        QStringList missing;
        for (const QString &code : codes)
            if (!skill.contains("`" + code + "`"))
                missing << code;
        QVERIFY2(missing.isEmpty(), qPrintable("not in the skill: " + missing.join(", ")));
        // And the other way: the skill names no code the spec does not.
        QRegularExpressionMatchIterator used = QRegularExpression("`([a-z]+(?:_[a-z]+)+)`").globalMatch(skill);
        while (used.hasNext()) {
            const QString code = used.next().captured(1);
            QVERIFY2(codes.contains(code), qPrintable(code + " is not a code of spec 13.3"));
        }
    }

    void skillsExampleWorkflowValidatesAndRuns() {
        const QString text = QString::fromUtf8(readFile(sourceSkill()));
        const int open = text.indexOf("```json fugue-example\n");
        QVERIFY(open >= 0);
        const int start = open + QString("```json fugue-example\n").size();
        const QByteArray example = text.mid(start, text.indexOf("\n```", start) - start).toUtf8();
        const QString file = m_dir.filePath("example.fugue");
        writeFile(file, example);
        QCOMPARE(fugue({"validate", file}).exitCode, 0);
        const Outcome run = fugue({"run", file}, m_dir.path());
        QCOMPARE(run.exitCode, 0);
        const QString id = run.data().value("run").toString();
        // Wait for it, as an agent would, by polling status.
        QJsonObject result;
        for (int i = 0; i < 100; ++i) {
            const Outcome status = fugue({"runs", "status", id});
            if (status.data().value("status").toString() == "ok") {
                result = fugue({"runs", "result", id}).data();
                break;
            }
            QTest::qWait(100);
        }
        QCOMPARE(result.value("outputs").toObject().value("show").toObject().value("value").toString(),
                 QString("a lighthouse, oil painting"));
    }
};

QTEST_GUILESS_MAIN(TestCliSkill)
#include "test_cli_skill.moc"
