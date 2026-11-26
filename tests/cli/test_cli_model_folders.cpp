// `fugue model find-folders` (spec 13.1, 8.9, #619) as the real binary over a fake install in a scratch HOME: it lists the same
// candidates as the first-run setup, only in the places named with --only (so no real drive is ever searched), and changes nothing.
// The finder's rules (caps, links, mounts) are tested in tests/engine/test_model_folder_finder.cpp.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <map>

class TestCliModelFolders : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    QString home() const { return m_dir.filePath("home"); }
    void touch(const QString &path, qint64 size = 4) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("abc");
        if (size > 3)
            file.resize(size);
    }
    std::map<QString, std::pair<qint64, qint64>> snapshot() const {
        std::map<QString, std::pair<qint64, qint64>> out;
        QDirIterator it(m_dir.path(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            out[it.filePath()] = {it.fileInfo().lastModified().toMSecsSinceEpoch(), it.fileInfo().size()};
        }
        return out;
    }
    QJsonObject run(const QStringList &args, int *exitCode = nullptr) const {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.insert("XDG_DATA_HOME", m_dir.filePath("data"));
        env.insert("XDG_STATE_HOME", m_dir.filePath("state"));
        env.insert("XDG_CACHE_HOME", m_dir.filePath("cache"));
        env.insert("XDG_CONFIG_HOME", m_dir.filePath("config"));
        env.insert("HOME", home());
        process.setProcessEnvironment(env);
        process.start(FUGUE_BINARY, QStringList{"model", "find-folders"} << args << "--json");
        process.closeWriteChannel();
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            return {};
        }
        if (exitCode)
            *exitCode = process.exitCode();
        return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    }

private slots:
    void itListsTheInstallsAndChangesNothing() {
        touch(home() + "/AI/ComfyUI/custom_nodes/x.py");
        touch(home() + "/AI/ComfyUI/models/checkpoints/a.safetensors", 3000000);
        for (int i = 0; i < 3; ++i)
            touch(home() + QString("/AI/ComfyUI/models/loras/l%1.safetensors").arg(i), 1000);
        touch(home() + "/AI/stable-diffusion-webui/webui.py");
        touch(home() + "/AI/stable-diffusion-webui/models/Stable-diffusion/x.safetensors");
        touch(home() + "/AI/stable-diffusion-webui/models/Lora/l.safetensors");
        const auto before = snapshot();
        int exitCode = -1;
        const QJsonObject reply = run({"--only", home()}, &exitCode);
        QCOMPARE(exitCode, 0);
        const QJsonArray folders = reply.value("data").toObject().value("folders").toArray();
        QCOMPARE(folders.size(), 2);
        QStringList tools;
        for (const QJsonValue &folder : folders)
            tools << folder.toObject().value("tool").toString();
        tools.sort();
        QCOMPARE(tools, (QStringList{"Automatic1111", "ComfyUI"}));
        for (const QJsonValue &value : folders) {
            const QJsonObject folder = value.toObject();
            if (folder.value("tool").toString() != "ComfyUI")
                continue;
            QCOMPARE(folder.value("models").toInt(), 4);
            QCOMPARE(folder.value("counts").toObject().value("fugue/lora").toInt(), 3);
            QCOMPARE(folder.value("size").toInteger(), qint64(3003000));
        }
        QVERIFY(!reply.value("data").toObject().value("capped").toBool());
        QVERIFY(snapshot() == before); // nothing written: not the home, not the settings
        QVERIFY(!QFileInfo::exists(m_dir.filePath("config/fugue/settings.json")));
    }

    void aPlaceItCannotSearchIsAUsageError() {
        int exitCode = -1;
        const QJsonObject reply = run({"--only", m_dir.filePath("nowhere")}, &exitCode);
        QCOMPARE(exitCode, 1);
        QCOMPARE(reply.value("errors").toArray().at(0).toObject().value("code").toString(), QStringLiteral("usage"));
    }
};

QTEST_GUILESS_MAIN(TestCliModelFolders)
#include "test_cli_model_folders.moc"
