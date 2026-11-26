// The first-run model folder setup at the model level (spec 8.9, #619; design: workbench first-run-models-ui.md, section 8, and the
// owner's decisions: a consent screen first, nothing searched until Search, an unticked place never touched, downloads default to the
// biggest writable folder picked). Seam: ModelSetup and SettingsModel over fake installs in a scratch home (small files, sparse sizes;
// the finder's world injected: its home, its drives and a probe that sees every path touched). No window, no real disk is scanned.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/fugue_home.h"
#include "engine/json.h"
#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "engine/registry.h"
#include "engine/settings.h"
#include "nodes/builtin.h"
#include "settings/settings_model.h"

using namespace fugue;

namespace {

const char *const kVariables[] = {"FUGUE_HOME", "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME",
                                  "FUGUE_BASE_XDG_STATE_HOME", "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR", "XDG_CONFIG_HOME",
                                  "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR"};

void touchFile(const QString &path, qint64 size = 4, const QByteArray &bytes = "abc") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    if (size > bytes.size())
        file.resize(size); // sparse
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A safetensors header naming these tensors, for the kind a folder's files vote.
QByteArray safetensors(const QStringList &tensors) {
    QJsonObject header;
    for (const QString &name : tensors)
        header.insert(name, QJsonObject{{"dtype", "F16"}, {"shape", QJsonArray{4, 4}}, {"data_offsets", QJsonArray{0, 0}}});
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out;
    for (int i = 0; i < 8; ++i)
        out.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
    return out + json;
}

QVariantMap rowOf(const QVariantList &rows, const QString &endsWith) {
    for (const QVariant &row : rows)
        if (row.toMap().value("path").toString().endsWith(endsWith))
            return row.toMap();
    return {};
}

} // namespace

class TestModelSetup : public QObject {
    Q_OBJECT

    QHash<QByteArray, QByteArray> m_saved;
    QSet<QByteArray> m_unset;
    QTemporaryDir m_dir;
    Registry m_registry;
    QString m_home, m_mnt;
    QStringList m_touched;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    FinderEnv env() {
        FinderEnv e;
        e.home = m_home;
        e.driveParents = {m_mnt};
        e.ownDataDir = path("base/data/fugue");
        e.touched = [this](const QString &p) { m_touched << p; };
        return e;
    }
    void comfy(const QString &root, int loras = 3, qint64 checkpoint = 2000000) {
        QDir().mkpath(root + "/custom_nodes");
        touchFile(root + "/models/checkpoints/a.safetensors", checkpoint);
        for (int i = 0; i < loras; ++i)
            touchFile(root + QString("/models/loras/l%1.safetensors").arg(i), 1000);
    }
    void a1111(const QString &root) {
        touchFile(root + "/webui.py");
        touchFile(root + "/models/Stable-diffusion/x.safetensors", 500000);
        touchFile(root + "/models/Lora/styleA.safetensors", 1000);
        touchFile(root + "/models/Lora/styleB.safetensors", 1000);
        touchFile(root + "/models/VAE/v.safetensors", 1000);
    }
    // Search to the end with the real finder.
    void searchAll(ModelSetup &setup) {
        setup.show(true);
        setup.search();
        QTRY_VERIFY(!setup.looking());
    }

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }
    void init() {
        for (const char *variable : kVariables) {
            if (qEnvironmentVariableIsSet(variable))
                m_saved.insert(variable, qgetenv(variable));
            else
                m_unset.insert(variable);
            qunsetenv(variable);
        }
        qputenv("XDG_CONFIG_HOME", path("base/config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", path("base/data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", path("base/state").toLocal8Bit());
        qputenv("XDG_RUNTIME_DIR", path("base/run").toLocal8Bit());
        QDir().mkpath(path("base/run"));
        m_home = path("home");
        m_mnt = path("mnt");
        QDir().mkpath(m_home);
        QDir().mkpath(m_mnt);
        m_touched.clear();
    }
    void cleanup() {
        for (auto it = m_saved.begin(); it != m_saved.end(); ++it)
            qputenv(it.key().constData(), it.value());
        for (const QByteArray &variable : std::as_const(m_unset))
            qunsetenv(variable.constData());
        m_saved.clear();
        m_unset.clear();
        for (const char *folder : {"base", "home", "mnt", "p"})
            QDir(path(folder)).removeRecursively();
    }

    // Check 2: after the yes, the A1111 root is read as ComfyUI's layout, and the downloads land in its own folder names.
    void anA1111InstallIsReadAfterTheYes() {
        a1111(m_home + "/stable-diffusion-webui");
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        searchAll(setup);
        const QVariantList rows = setup.rows();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value("title").toString(), QStringLiteral("Automatic1111"));
        QVERIFY(rows.first().toMap().value("on").toBool());
        QSignalSpy changed(&settings, &SettingsModel::changed);
        QSignalSpy applied(&setup, &ModelSetup::applied);
        setup.apply();
        QCOMPARE(applied.size(), 1);
        QCOMPARE(changed.size(), 1); // one write
        const QString models = m_home + "/stable-diffusion-webui/models";
        const SettingsFile file = readSettings(settingsPath());
        QCOMPARE(file.settings.modelPaths, QStringList{models});
        QCOMPARE(file.settings.modelFolderKinds.value(models), ModelLibrary::kLayout);
        QCOMPARE(file.settings.modelsSetup, QStringLiteral("done"));
        QCOMPARE(file.settings.downloadFolder, models); // the biggest writable folder picked
        const ModelLibrary library = ModelLibrary::current(&m_registry);
        QCOMPARE(library.list(*PortType::parse("fugue/lora")).size(), 2);
        QCOMPARE(library.list(*PortType::parse("fugue/model")).size(), 1);
        QCOMPARE(library.downloadFolder(*PortType::parse("fugue/vae")), models + "/VAE");
        QVERIFY(!setup.isOpen());
    }

    // Check 9: when it opens by itself.
    void itOpensOnlyWhenTheFourConditionsHold() {
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        QVERIFY(setup.shouldOffer()); // an empty home
        QVERIFY(setup.maybeOffer());
        QVERIFY(setup.isOpen());
        setup.skip();
        QCOMPARE(readSettings(settingsPath()).settings.modelsSetup, QStringLiteral("skipped")); // the key is set: never again
        QVERIFY(!setup.shouldOffer());
        QFile::remove(settingsPath());
        settings.reload();
        QVERIFY(setup.shouldOffer());
        // A model_paths entry.
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{path("home/models")}).isEmpty());
        QVERIFY(!setup.shouldOffer());
        QFile::remove(settingsPath());
        settings.reload();
        // A model in Fugue's own folder.
        touchFile(dataDir() + "/models/checkpoints/own.safetensors");
        QVERIFY(!setup.shouldOffer());
        QFile::remove(dataDir() + "/models/checkpoints/own.safetensors");
        QVERIFY(setup.shouldOffer());
    }
    void itNeverOpensByItselfInAProject() {
        ProjectManifest manifest;
        manifest.name = "Acme";
        manifest.inherits = {"models"};
        QVERIFY(QDir().mkpath(path("p/acme")));
        QVERIFY(writeProject(path("p/acme"), manifest).isEmpty());
        QVERIFY(setFugueHome(path("p/acme"), nullptr));
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        QVERIFY(!settings.modelSetup()->shouldOffer());
        QVERIFY(!settings.modelSetup()->maybeOffer());
        QVERIFY(!settings.modelSetup()->isOpen());
        // A rerun is the way in.
        settings.modelSetup()->show(false);
        QVERIFY(settings.modelSetup()->isOpen());
    }

    // Check 8: Skip writes only "skipped"; a rerun's Escape writes nothing; Done with nothing found writes only "done".
    void skipAndDoneWriteOnlyTheirKey() {
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.show(true);
        setup.skip();
        QCOMPARE(readJsonObject(settingsPath()).value(), (QJsonObject{{"models_setup", "skipped"}}));
        QFile::remove(settingsPath());
        settings.reload();
        setup.show(false); // a rerun
        setup.skip();
        QVERIFY(!QFileInfo::exists(settingsPath()));
        // Nothing found: Done.
        setup.setEnv(env());
        setup.show(true);
        setup.search();
        QTRY_VERIFY(!setup.looking());
        QVERIFY(setup.nothing());
        QCOMPARE(setup.primaryText(), QStringLiteral("Done"));
        setup.apply();
        QCOMPARE(readJsonObject(settingsPath()).value(), (QJsonObject{{"models_setup", "done"}}));
    }

    // Check 4 and the owner's rule: only Search searches, nothing is changed, and an unticked place is never touched.
    void nothingIsTouchedBeforeSearchAndAnUntickedPlaceNever() {
        comfy(m_home + "/ComfyUI");
        a1111(m_mnt + "/d/stable-diffusion-webui");
        touchFile(path("home/hashes-before"), 4);
        const QString hashes = dataDir() + "/hashes.json";
        touchFile(hashes);
        const QByteArray hashesBefore = readAll(hashes);
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        setup.show(true);
        QVERIFY(!setup.locations().isEmpty());
        QStringList ids;
        for (const QVariant &location : setup.locations())
            ids << location.toMap().value("id").toString();
        QVERIFY(ids.contains("drive:" + m_mnt));
        QVERIFY(ids.contains("probe:" + m_home + "/ComfyUI/models"));
        setup.setLocationChecked("drive:" + m_mnt, false);
        setup.setLocationChecked("probe:" + m_home + "/ComfyUI/models", false);
        m_touched.clear(); // showing the screen only checked that each place exists
        QTest::qWait(30);
        QVERIFY2(m_touched.isEmpty(), "nothing is touched before Search");
        QVERIFY(setup.canSearch());
        setup.search();
        QTRY_VERIFY(!setup.looking());
        for (const QString &touched : std::as_const(m_touched)) {
            QVERIFY2(!touched.startsWith(m_mnt), qPrintable("touched " + touched));
            QVERIFY2(touched != m_home + "/ComfyUI/models" && !touched.startsWith(m_home + "/ComfyUI/models/"), qPrintable("touched " + touched));
        }
        QVERIFY(setup.rows().isEmpty()); // both finds were in unticked places
        // Nothing ticked: Search is off.
        setup.skip();
        setup.show(false);
        for (const QVariant &location : setup.locations())
            setup.setLocationChecked(location.toMap().value("id").toString(), false);
        QVERIFY(!setup.canSearch());
        QVERIFY(!setup.primaryEnabled());
        setup.skip();
        QCOMPARE(readAll(hashes), hashesBefore);
    }

    // The owner's rule for downloads: the picked writable folder with the most models by size, not the most files; Fugue's own folder
    // when none can be written; a short disk keeps the choice and says so.
    void downloadsGoToTheBiggestWritableFolderPicked() {
        comfy(m_home + "/ComfyUI", 40, 1000);            // 41 files, small
        a1111(m_mnt + "/d/stable-diffusion-webui");       // 4 files, 503 kB: the biggest
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        searchAll(setup);
        QCOMPARE(setup.rows().size(), 2);
        QCOMPARE(setup.rows().first().toMap().value("title").toString(), QStringLiteral("ComfyUI")); // listed by count
        const QString big = m_mnt + "/d/stable-diffusion-webui/models";
        const QString small = m_home + "/ComfyUI/models";
        QCOMPARE(setup.download(), big);
        QVariantList choices = setup.choices();
        QCOMPARE(choices.size(), 3); // Fugue's own, then each picked folder
        QCOMPARE(choices.at(0).toMap().value("value").toString(), dataDir() + "/models");
        QCOMPARE(choices.at(1).toMap().value("value").toString(), big);
        // Untick the big one: the next biggest is the default; untick both: Fugue's own.
        setup.setRowOn(big, false);
        QCOMPARE(setup.download(), small);
        QCOMPARE(setup.choices().size(), 2);
        setup.setRowOn(small, false);
        QCOMPARE(setup.download(), dataDir() + "/models");
        setup.setRowOn(big, true);
        // A short disk keeps the choice and says it plainly; it never switches silently.
        setup.setFreeSpace([&](const QString &folder) { return folder.startsWith(m_mnt) ? qint64(3000000000) : qint64(900000000000); });
        QCOMPARE(setup.download(), big);
        QVERIFY(setup.downloadLow());
        QVERIFY2(setup.downloadWarning().contains("3"), qPrintable(setup.downloadWarning()));
        QVERIFY(setup.primaryEnabled());
        // The person can pick another from the list.
        setup.setDownload(dataDir() + "/models");
        QCOMPARE(setup.download(), dataDir() + "/models");
        QVERIFY(!setup.downloadLow() || setup.downloadWarning().isEmpty() == false);
        setup.setDownload(big);
        setup.apply();
        QCOMPARE(readSettings(settingsPath()).settings.downloadFolder, big);
    }

    // Check 7: a read-only folder is in neither the choices nor the default.
    void aReadOnlyFolderIsNeverADownloadTarget() {
        a1111(m_home + "/stable-diffusion-webui");
        const QString models = m_home + "/stable-diffusion-webui/models";
        QVERIFY(QFile::setPermissions(models, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        if (QFileInfo(models).isWritable()) {
            QFile::setPermissions(models, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            QSKIP("running as a user that can write anywhere");
        }
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        searchAll(setup);
        QVERIFY(rowOf(setup.rows(), "stable-diffusion-webui/models").value("readOnly").toBool());
        QCOMPARE(setup.choices().size(), 1);
        QCOMPARE(setup.download(), dataDir() + "/models");
        QFile::setPermissions(models, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    // Check 10: in a project a rerun writes the project's own file; the Default's bytes are unchanged and its roots are not offered.
    void aRerunInAProjectWritesOnlyTheProjectsFile() {
        QDir().mkpath(path("base/config/fugue"));
        const QString shared = path("home/shared-models");
        QDir().mkpath(shared);
        QVERIFY(changeSetting(path("base/config/fugue/settings.json"), "model_paths", QJsonArray{shared}).isEmpty());
        const QByteArray defaultBefore = readAll(path("base/config/fugue/settings.json"));
        ProjectManifest manifest;
        manifest.name = "Acme";
        manifest.inherits = {"models"};
        QVERIFY(QDir().mkpath(path("p/acme")));
        QVERIFY(writeProject(path("p/acme"), manifest).isEmpty());
        QVERIFY(setFugueHome(path("p/acme"), nullptr));
        comfy(m_home + "/ComfyUI");
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        setup.show(false);
        setup.search();
        QTRY_VERIFY(!setup.looking());
        const QVariantMap inherited = rowOf(setup.rows(), "shared-models");
        QVERIFY(inherited.value("inherited").toBool());
        QVERIFY(inherited.value("inUse").toBool());
        for (const QVariant &choice : setup.choices())
            QVERIFY(choice.toMap().value("value").toString() != shared); // never a download target
        setup.apply();
        QCOMPARE(readAll(path("base/config/fugue/settings.json")), defaultBefore);
        const SettingsFile own = readSettings(settingsPath());
        QVERIFY(settingsPath().startsWith(path("p/acme")));
        QCOMPARE(own.settings.modelPaths, QStringList{m_home + "/ComfyUI/models"});
        QVERIFY(own.settings.modelsSetup.isEmpty()); // a rerun writes no models_setup
        QCOMPARE(own.settings.downloadFolder, m_home + "/ComfyUI/models");
    }

    // Check 11: a template's model whose file name is now in an added folder is ready (the check templates() makes: remembered hashes
    // only, no file is read).
    void aTemplatesModelIsReadyOnceItsFolderIsAdded() {
        comfy(m_home + "/ComfyUI");
        touchFile(m_home + "/ComfyUI/models/checkpoints/wanted-model.safetensors", 100);
        const QJsonArray models{QJsonObject{{"resource", "fugue/model"},
                                            {"name", "wanted-model.safetensors"},
                                            {"hash", "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}}};
        FileHashes hashes(FileHashes::defaultPath());
        QCOMPARE(ModelLibrary::current(&m_registry).missing(models, hashes).size(), 1);
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        searchAll(setup);
        setup.apply();
        QCOMPARE(ModelLibrary::current(&m_registry).missing(models, hashes).size(), 0);
    }

    // A folder the person adds that Fugue cannot tell: the primary waits for the choice.
    void aFolderItCannotTellNeedsAChoice() {
        const QString mystery = path("home/mystery");
        touchFile(mystery + "/one.safetensors", 100);
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        setup.show(true);
        QCOMPARE(setup.addFolder(mystery), QString()); // a manual pick: no search
        QCOMPARE(setup.phase(), QStringLiteral("results"));
        QVERIFY(rowOf(setup.rows(), "mystery").value("needsKind").toBool());
        QVERIFY(!setup.primaryEnabled());
        QCOMPARE(setup.addFolder(mystery), QStringLiteral("That folder is already in the list."));
        QVERIFY(!setup.addFolder(path("home/none")).isEmpty());
        setup.setRowKind(mystery, "fugue/lora");
        QVERIFY(setup.primaryEnabled());
        setup.apply();
        QCOMPARE(readSettings(settingsPath()).settings.modelFolderKinds.value(mystery), QStringLiteral("fugue/lora"));
    }

    // InvokeAI is off by default; when ticked, the folders it can tell apart are added single-kind.
    void invokeAiIsOffAndWhenTickedIsAddedByTheFoldersItCanTell() {
        touchFile(m_home + "/invokeai/invokeai.yaml");
        const QStringList lora{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight", "diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight"};
        touchFile(m_home + "/invokeai/models/sdxl/lora/one.safetensors", 0, safetensors(lora));
        touchFile(m_home + "/invokeai/models/core/odd.safetensors", 0, safetensors({"something.else"}));
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        searchAll(setup);
        const QVariantMap row = rowOf(setup.rows(), "invokeai/models");
        QVERIFY(!row.isEmpty());
        QVERIFY(!row.value("on").toBool());
        setup.setRowOn(row.value("path").toString(), true);
        QSignalSpy applied(&setup, &ModelSetup::applied);
        setup.apply();
        QCOMPARE(applied.size(), 1);
        QVERIFY2(applied.first().first().toString().contains("left out"), qPrintable(applied.first().first().toString()));
        const SettingsFile file = readSettings(settingsPath());
        QCOMPARE(file.settings.modelPaths, QStringList{m_home + "/invokeai/models/sdxl/lora"});
        QCOMPARE(file.settings.modelFolderKinds.value(m_home + "/invokeai/models/sdxl/lora"), QStringLiteral("fugue/lora"));
    }

    // A folder in use is listed first, on, and turning it off removes it at the yes.
    void aFolderInUseCanBeTurnedOff() {
        comfy(m_home + "/ComfyUI");
        SettingsModel settings;
        settings.attach(nullptr, nullptr, &m_registry);
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{m_home + "/ComfyUI/models"}).isEmpty());
        QVERIFY(changeSetting(settingsPath(), "model_folder_kinds", QJsonObject{{m_home + "/ComfyUI/models", "layout"}}).isEmpty());
        settings.reload();
        ModelSetup &setup = *settings.modelSetup();
        setup.setEnv(env());
        setup.show(false);
        setup.search();
        QTRY_VERIFY(!setup.looking());
        QCOMPARE(setup.rows().size(), 1); // not listed twice
        const QVariantMap row = setup.rows().first().toMap();
        QVERIFY(row.value("inUse").toBool() && row.value("on").toBool());
        setup.setRowOn(row.value("path").toString(), false);
        setup.apply();
        QVERIFY(readSettings(settingsPath()).settings.modelPaths.isEmpty());
        QVERIFY(readSettings(settingsPath()).settings.modelFolderKinds.isEmpty());
    }
};

QTEST_MAIN(TestModelSetup)
#include "test_model_setup.moc"
