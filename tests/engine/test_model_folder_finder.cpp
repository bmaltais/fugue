// Finding the model folders a person has (spec 8.9, #619): the detector over fake installs in a scratch home.
//
// Seam: findModelFolders() with an injected FinderEnv (the home, the drives, the mounts, the clock and a probe that sees every
// path touched). The registry's resource types come from the real pack.json, so the folder names of A1111, Forge, SwarmUI and
// Stability Matrix are the shipped ones. Files are tiny or sparse; none is ever read.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <map>

#include "engine/model_folder_finder.h"
#include "engine/model_library.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

void touchFile(const QString &path, qint64 size = 4) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("abc");
    if (size > 3)
        file.resize(size); // sparse
}

// path -> (mtime, size) of every file below `root`.
std::map<QString, std::pair<qint64, qint64>> snapshot(const QString &root) {
    std::map<QString, std::pair<qint64, qint64>> out;
    QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        out[it.filePath()] = {it.fileInfo().lastModified().toMSecsSinceEpoch(), it.fileInfo().size()};
    }
    return out;
}

} // namespace

class TestModelFolderFinder : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_registry;
    QString m_home, m_mnt;
    QStringList m_touched;

    FinderEnv env() {
        FinderEnv e;
        e.home = m_home;
        e.driveParents = {m_mnt};
        e.ownDataDir = m_home + "/.local/share/fugue";
        e.touched = [this](const QString &path) { m_touched << path; };
        return e;
    }
    QList<FinderLocation> all(const FinderEnv &e) { return finderLocations(e); }
    FindResult find(const FinderEnv &e, const QList<FinderLocation> &locations, const QStringList &skip = {}) {
        FinderEnv quiet = e; // what was offered is told apart from what is searched: the probe sees only the search
        quiet.touched = nullptr;
        return findModelFolders(m_registry, locations, e, skip, {}, finderLocations(quiet));
    }
    QList<FinderLocation> only(const FinderEnv &e, const QStringList &ids) {
        QList<FinderLocation> list;
        for (const FinderLocation &location : finderLocations(e))
            if (ids.contains(location.id))
                list << location;
        return list;
    }

    void a1111(const QString &root) {
        touchFile(root + "/webui.py");
        touchFile(root + "/models/Stable-diffusion/x.safetensors");
        touchFile(root + "/models/Lora/l.safetensors");
    }
    void comfy(const QString &root) {
        QDir().mkpath(root + "/custom_nodes");
        touchFile(root + "/models/checkpoints/a.safetensors", 2000000);
        for (int i = 0; i < 3; ++i)
            touchFile(root + QString("/models/loras/l%1.safetensors").arg(i), 1000);
        touchFile(root + "/models/vae/ae.safetensors");
    }

private slots:
    void initTestCase() {
        QFile pack(QStringLiteral(FUGUE_SOURCE_DIR "/src/nodes/manifests/pack.json"));
        QVERIFY(pack.open(QIODevice::ReadOnly));
        for (const QJsonValue &value : QJsonDocument::fromJson(pack.readAll()).object().value("types").toArray()) {
            const QJsonObject object = value.toObject();
            if (object.value("kind").toString() != "resource")
                continue;
            DeclaredPortType declared;
            declared.type = *PortType::parse(object.value("id").toString());
            declared.kind = PortKind::Resource;
            declared.name = object.value("name").toString();
            for (const QJsonValue &folder : object.value("folders").toArray())
                declared.folders << folder.toString();
            for (const QJsonValue &ext : object.value("extensions").toArray())
                declared.extensions << ext.toString();
            QVERIFY(m_registry.add(declared).isEmpty());
        }
    }
    void init() {
        m_dir.remove();
        QVERIFY(m_dir.isValid());
        m_home = m_dir.path() + "/home";
        m_mnt = m_dir.path() + "/mnt";
        QDir().mkpath(m_home);
        QDir().mkpath(m_mnt);
        m_touched.clear();
    }

    // Check 1 of the design.
    void aComfyUiTreeIsOneCandidateWithItsCounts() {
        comfy(m_home + "/ComfyUI");
        const FinderEnv e = env();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        const FoundFolder &folder = result.folders.first();
        QCOMPARE(folder.tool, QStringLiteral("ComfyUI"));
        QCOMPARE(folder.path, m_home + "/ComfyUI/models");
        QCOMPARE(folder.counts.value("fugue/model"), 1);
        QCOMPARE(folder.counts.value("fugue/lora"), 3);
        QCOMPARE(folder.counts.value("fugue/vae"), 1);
        QCOMPARE(folder.models, 5);
        QCOMPARE(folder.size, qint64(2000000 + 3000 + 4));
        QVERIFY(!folder.countCapped && !folder.network && folder.layout);
    }

    // The folder-name gap: A1111, Forge, SwarmUI and Stability Matrix use names the pack.json now lists.
    void theNamesOfTheOtherToolsAreRead() {
        const QString a1111 = m_home + "/stable-diffusion-webui";
        touchFile(a1111 + "/webui.py");
        touchFile(a1111 + "/models/Stable-diffusion/x.safetensors");
        touchFile(a1111 + "/models/Lora/l.safetensors");
        touchFile(a1111 + "/models/VAE/v.safetensors");
        const QString forge = m_home + "/stable-diffusion-webui-forge";
        touchFile(forge + "/webui.py");
        touchFile(forge + "/modules_forge/x.py");
        touchFile(forge + "/models/Stable-diffusion/x.safetensors");
        touchFile(forge + "/models/Lora/l.safetensors");
        const QString swarm = m_home + "/SwarmUI";
        touchFile(swarm + "/launch-linux.sh");
        touchFile(swarm + "/Models/Stable-Diffusion/x.safetensors");
        touchFile(swarm + "/Models/Lora/l.safetensors");
        const QString matrix = m_home + "/StabilityMatrix";
        QDir().mkpath(matrix + "/Data/Packages");
        touchFile(matrix + "/Data/Models/StableDiffusion/x.safetensors");
        touchFile(matrix + "/Data/Models/Lora/l.safetensors");
        touchFile(matrix + "/Data/Models/TextEncoders/t.safetensors");
        const FinderEnv e = env();
        QStringList tools;
        for (const FoundFolder &folder : find(e, all(e)).folders)
            tools << folder.tool + "@" + QFileInfo(folder.path).fileName();
        tools.sort();
        QCOMPARE(tools, (QStringList{"Automatic1111@models", "Forge@models", "Stability Matrix@Models", "SwarmUI@Models"}));
        // The library reads such a root as ComfyUI's layout: its LoRAs and checkpoints are found.
        ModelLibrary library({a1111 + "/models"}, &m_registry, {{a1111 + "/models", ModelLibrary::kLayout}});
        QCOMPARE(library.list(*PortType::parse("fugue/lora")).size(), 1);
        QCOMPARE(library.list(*PortType::parse("fugue/model")).size(), 1);
        QVERIFY(library.downloadFolder(*PortType::parse("fugue/vae")).endsWith("models/VAE"));
    }

    void theToolIsNamedFromItsSiblings() {
        const FinderEnv e = env();
        const QString portable = m_dir.path() + "/ComfyUI_windows_portable";
        QDir().mkpath(portable + "/python_embeded");
        QDir().mkpath(portable + "/ComfyUI/custom_nodes");
        QDir().mkpath(portable + "/ComfyUI/models");
        QCOMPARE(finderToolName(portable + "/ComfyUI/models", e), QStringLiteral("ComfyUI portable"));
        QCOMPARE(finderToolName(m_home + "/ComfyUI-Shared/models", e), QStringLiteral("ComfyUI Desktop"));
        QCOMPARE(finderToolName(m_dir.path() + "/lone/models", e), QStringLiteral("Model folder"));
    }

    void invokeAiIsToldByItsYamlAndIsNotALayout() {
        touchFile(m_home + "/invokeai/invokeai.yaml");
        touchFile(m_home + "/invokeai/models/sdxl/main/x.safetensors", 5000);
        touchFile(m_home + "/invokeai/models/core/y.safetensors", 5000);
        const FinderEnv e = env();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QCOMPARE(result.folders.first().tool, QStringLiteral("InvokeAI"));
        QVERIFY(!result.folders.first().layout);
        QCOMPARE(result.folders.first().models, 2);
    }

    // Check 3: one root by two links is one row; Stability Matrix linked into ComfyUI is one row.
    void aRootReachedTwiceIsOneRowWithItsOtherPath() {
        comfy(m_home + "/ComfyUI");
        QVERIFY(QFile::link(m_home + "/ComfyUI", m_home + "/shortcut"));
        const FinderEnv e = env();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QVERIFY2(result.folders.first().alsoReached.contains(m_home + "/shortcut/models"), qPrintable(result.folders.first().alsoReached.join(",")));
    }
    void stabilityMatrixLinkedIntoComfyUiIsOneRow() {
        comfy(m_home + "/ComfyUI");
        const QString shared = m_home + "/StabilityMatrix/Data/Models";
        QDir().mkpath(shared);
        QDir().mkpath(m_home + "/StabilityMatrix/Data/Packages");
        QVERIFY(QFile::link(m_home + "/ComfyUI/models/checkpoints", shared + "/StableDiffusion"));
        QVERIFY(QFile::link(m_home + "/ComfyUI/models/loras", shared + "/Lora"));
        const FinderEnv e = env();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QCOMPARE(result.folders.first().tool, QStringLiteral("ComfyUI"));
        QCOMPARE(result.folders.first().models, 5); // each file once
        QVERIFY(result.folders.first().alsoReached.contains(shared));
    }

    // Check 4: nothing is changed or hashed.
    void aScanChangesNothing() {
        comfy(m_home + "/ComfyUI");
        touchFile(m_mnt + "/c/Users/me/Documents/ComfyUI/custom_nodes/x.py");
        touchFile(m_mnt + "/c/Users/me/Documents/ComfyUI/models/checkpoints/b.safetensors", 100000);
        touchFile(m_mnt + "/c/Users/me/Documents/ComfyUI/models/loras/b.safetensors", 100000);
        const QString hashes = m_home + "/.local/share/fugue/hashes.json";
        touchFile(hashes);
        const QByteArray hashesBefore = [&] { QFile f(hashes); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        const auto before = snapshot(m_dir.path());
        const FinderEnv e = env();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 2); // the Windows drive's too
        QVERIFY(snapshot(m_dir.path()) == before);
        QFile f(hashes);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), hashesBefore);
        // No model file is opened: only directories are listed, files only stat'd (their path is touched, never read).
    }

    // Check 5: the caps.
    void theTimeCapStopsFindingAndSaysSo() {
        comfy(m_home + "/ComfyUI");
        for (int i = 0; i < 40; ++i)
            QDir().mkpath(m_home + QString("/d%1/sub").arg(i));
        FinderEnv e = env();
        qint64 clock = 0;
        e.nowMs = [&clock] { return clock; };
        int listed = 0;
        e.touched = [&](const QString &path) { clock += 1000; if (path.contains(QRegularExpression("/d[0-9]+$"))) ++listed; }; // every listing costs a second
        const FindResult result = find(e, all(e));
        QVERIFY(result.capped);
        QVERIFY2(listed < 10, qPrintable(QString::number(listed))); // it stopped after a few, not after all 40 folders
    }
    void aCountStopsAtItsEntryCapAndSaysSo() {
        comfy(m_home + "/ComfyUI");
        for (int i = 0; i < 120; ++i)
            touchFile(m_home + QString("/ComfyUI/models/loras/more%1.safetensors").arg(i), 10);
        FinderEnv e = env();
        e.countCapEntries = 50;
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QVERIFY(result.folders.first().countCapped);
        QVERIFY(result.folders.first().models <= 50);
    }
    void aCountStopsAtItsTimeCap() {
        comfy(m_home + "/ComfyUI");
        FinderEnv e = env();
        qint64 clock = 0;
        e.nowMs = [&clock] { return clock; };
        e.findCapMs = 1000000;
        int touches = 0;
        e.touched = [&](const QString &path) { if (path.contains("/models/loras")) clock += 3000; ++touches; };
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QVERIFY(result.folders.first().countCapped);
    }

    // Check 6: a network drive is never walked.
    void aNetworkDriveIsNotWalked() {
        touchFile(m_mnt + "/nas/ComfyUI/custom_nodes/x.py");
        touchFile(m_mnt + "/nas/ComfyUI/models/checkpoints/a.safetensors");
        touchFile(m_mnt + "/nas/ComfyUI/models/loras/a.safetensors");
        FinderEnv e = env();
        const QByteArray info = "36 29 0:31 / " + m_mnt.toUtf8() + "/nas rw,relatime shared:1 - cifs //server/share rw\n"
                                "37 29 0:32 / /boot rw - vfat /dev/sda1 rw\n";
        e.networkMounts = networkMountsFrom(info);
        QCOMPARE(e.networkMounts, QStringList{m_mnt + "/nas"});
        const FindResult result = find(e, all(e));
        QVERIFY(result.folders.isEmpty());
        for (const QString &path : std::as_const(m_touched))
            QVERIFY2(!path.startsWith(m_mnt + "/nas/"), qPrintable(path));
    }
    void anInstallOnANetworkDriveIsListedNotCounted() {
        comfy(m_home + "/ComfyUI");
        FinderEnv e = env();
        e.networkMounts = {m_home};
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QVERIFY(result.folders.first().network);
        QCOMPARE(result.folders.first().models, 0);
    }

    // Unticked places are never touched.
    void anUntickedLocationIsNeverTouched() {
        comfy(m_home + "/ComfyUI");
        touchFile(m_mnt + "/c/ComfyUI/custom_nodes/x.py");
        touchFile(m_mnt + "/c/ComfyUI/models/checkpoints/a.safetensors");
        touchFile(m_mnt + "/c/ComfyUI/models/loras/a.safetensors");
        const FinderEnv e = env();
        // Everything but /mnt and the exact ComfyUI folder.
        QStringList ids;
        for (const FinderLocation &location : finderLocations(e))
            if (location.id != "drive:" + m_mnt && location.id != "probe:" + m_home + "/ComfyUI/models")
                ids << location.id;
        const QList<FinderLocation> ticked = only(e, ids);
        m_touched.clear();
        const FindResult result = find(e, ticked);
        QVERIFY(result.folders.isEmpty() || result.folders.first().path != m_mnt + "/c/ComfyUI/models");
        for (const QString &path : std::as_const(m_touched)) {
            QVERIFY2(!path.startsWith(m_mnt), qPrintable("touched " + path));
            QVERIFY2(path != m_home + "/ComfyUI/models" && !path.startsWith(m_home + "/ComfyUI/models/"), qPrintable("touched " + path));
        }
        // And with nothing ticked, nothing at all is touched.
        m_touched.clear();
        QVERIFY(find(e, {}).folders.isEmpty());
        QVERIFY2(m_touched.isEmpty(), qPrintable(m_touched.join(",")));
    }

    void theLocationsAreOnlyThosePresent() {
        const FinderEnv e = env();
        QStringList paths;
        for (const FinderLocation &location : finderLocations(e))
            paths << location.display;
        QCOMPARE(paths, (QStringList{"~", m_mnt})); // no ComfyUI folder yet: not listed
        comfy(m_home + "/ComfyUI");
        paths.clear();
        for (const FinderLocation &location : finderLocations(e))
            paths << location.display;
        QVERIFY(paths.contains("~/ComfyUI/models"));
    }

    void aFolderOfNoModelsAndNoToolIsACoincidenceOfNames() {
        QDir().mkpath(m_home + "/src/vae");
        QDir().mkpath(m_home + "/src/lora");
        QDir().mkpath(m_home + "/ComfyUI/custom_nodes");
        QDir().mkpath(m_home + "/ComfyUI/models/checkpoints");
        QDir().mkpath(m_home + "/ComfyUI/models/loras");
        const FinderEnv e = env();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1); // the ComfyUI install with no models yet is listed (off); the coincidence is not
        QCOMPARE(result.folders.first().tool, QStringLiteral("ComfyUI"));
        QCOMPARE(result.folders.first().models, 0);
    }

    void aFolderToSkipIsLeftOut() {
        comfy(m_home + "/ComfyUI");
        const FinderEnv e = env();
        QVERIFY(find(e, all(e), {m_home + "/ComfyUI/models"}).folders.isEmpty());
    }

    void aPhotoFolderOfThousandsOfEntriesIsNotWalked() {
        FinderEnv e = env();
        e.tooManyEntries = 30;
        QDir().mkpath(m_home + "/photos");
        for (int i = 0; i < 40; ++i)
            QDir().mkpath(m_home + QString("/photos/p%1").arg(i));
        touchFile(m_home + "/photos/p3/ComfyUI/custom_nodes/x.py");
        touchFile(m_home + "/photos/p3/ComfyUI/models/checkpoints/a.safetensors");
        touchFile(m_home + "/photos/p3/ComfyUI/models/loras/a.safetensors");
        QVERIFY(find(e, all(e)).folders.isEmpty());
    }

    // A link into a place that is not ticked is never followed: not at the top, not nested, not to an unticked install folder. The probe
    // records resolved paths too, so a stat of the target would show.
    void aLinkIntoAnUntickedPlaceIsNeverFollowed() {
        comfy(m_mnt + "/ssd/ComfyUI");
        QVERIFY(QFile::link(m_mnt + "/ssd/ComfyUI/models", m_home + "/models"));
        QDir().mkpath(m_home + "/a/b");
        QVERIFY(QFile::link(m_mnt + "/ssd", m_home + "/a/b/ssd"));
        comfy(m_home + "/ComfyUI"); // an unticked install folder, reached by another link
        QVERIFY(QFile::link(m_home + "/ComfyUI", m_home + "/shortcut"));
        QVERIFY(QFile::link(m_home + "/ComfyUI/models", m_home + "/shortcut-models"));
        const FinderEnv e = env();
        QStringList ids;
        for (const FinderLocation &location : finderLocations(e))
            if (location.id != "drive:" + m_mnt && location.id != "probe:" + m_home + "/ComfyUI/models")
                ids << location.id;
        const QList<FinderLocation> ticked = only(e, ids);
        m_touched.clear();
        const FindResult result = find(e, ticked);
        QVERIFY(result.folders.isEmpty());
        for (const QString &path : std::as_const(m_touched)) {
            QVERIFY2(!path.startsWith(m_mnt + "/"), qPrintable("touched " + path));
            QVERIFY2(!path.startsWith(m_home + "/ComfyUI/"), qPrintable("touched " + path));
        }
        // With the places ticked the same links are found, each tree once.
        m_touched.clear();
        const FindResult all = find(e, finderLocations(e));
        QCOMPARE(all.folders.size(), 2);
    }

    // An automount is not stat'd into: touching it would mount the share. It is in the network set, and is not even a listed place.
    void anAutomountIsNeverTouched() {
        const QByteArray info = "50 29 0:40 / " + m_mnt.toUtf8() + "/nas rw,relatime shared:1 - autofs systemd-1 rw,fd=30\n"
                                "51 50 0:41 / " + m_mnt.toUtf8() + "/nas/share rw - nfs4 srv:/x rw\n";
        FinderEnv e = env();
        e.networkMounts = networkMountsFrom(info);
        e.autofsMounts = autofsMountsFrom(info);
        QCOMPARE(e.autofsMounts, QStringList{m_mnt + "/nas"});
        QVERIFY(e.networkMounts.contains(m_mnt + "/nas"));
        QVERIFY(e.networkMounts.contains(m_mnt + "/nas/share"));
        touchFile(m_mnt + "/nas/ComfyUI/custom_nodes/x.py");
        touchFile(m_mnt + "/nas/ComfyUI/models/checkpoints/a.safetensors");
        touchFile(m_mnt + "/nas/ComfyUI/models/loras/a.safetensors");
        touchFile(m_mnt + "/c/ComfyUI/custom_nodes/x.py");
        touchFile(m_mnt + "/c/ComfyUI/models/checkpoints/a.safetensors");
        touchFile(m_mnt + "/c/ComfyUI/models/loras/a.safetensors");
        QVERIFY(QFile::link(m_mnt + "/nas/ComfyUI/models", m_home + "/linked-models"));
        m_touched.clear();
        const FindResult result = find(e, finderLocations(e));
        QCOMPARE(result.folders.size(), 1); // the drive that is not an automount
        for (const QString &path : std::as_const(m_touched))
            QVERIFY2(!path.startsWith(m_mnt + "/nas/"), qPrintable("touched " + path));
    }

    // A type folder that is a link to a network drive is not counted (and nothing below it is listed).
    void aLinkedNetworkFolderIsNotCounted() {
        comfy(m_home + "/ComfyUI");
        touchFile(m_mnt + "/nas/loras/far.safetensors", 100);
        QVERIFY(QDir(m_home + "/ComfyUI/models/loras").removeRecursively());
        QVERIFY(QFile::link(m_mnt + "/nas/loras", m_home + "/ComfyUI/models/loras"));
        FinderEnv e = env();
        e.networkMounts = {m_mnt + "/nas"};
        m_touched.clear();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QCOMPARE(result.folders.first().counts.value("fugue/lora"), 0);
        for (const QString &path : std::as_const(m_touched))
            QVERIFY2(!path.startsWith(m_mnt + "/nas"), qPrintable("touched " + path));
    }

    // The 2 s cap is checked at every entry, in the one big folder as well, and says so.
    void aSingleBigFolderStopsAtTheTimeCapInsideTheLoop() {
        comfy(m_home + "/ComfyUI");
        for (int i = 0; i < 200; ++i)
            touchFile(m_home + QString("/ComfyUI/models/loras/more%1.safetensors").arg(i, 3, 10, QChar('0')), 10);
        FinderEnv e = env();
        qint64 clock = 0;
        e.nowMs = [&clock] { return clock; };
        e.findCapMs = 1000000;
        e.totalCountCapMs = 1000000;
        e.touched = [&clock](const QString &path) { if (path.contains("/loras/more")) clock += 100; }; // each file stat costs 100 ms
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QVERIFY(result.folders.first().countCapped);
        QVERIFY2(result.folders.first().counts.value("fugue/lora") < 40, qPrintable(QString::number(result.folders.first().counts.value("fugue/lora"))));
    }

    // Counting every folder has a total too: past it the rest are listed with a count that stopped, and the search says it ended early.
    void theTotalForCountingIsCapped() {
        comfy(m_home + "/ComfyUI");
        a1111(m_home + "/stable-diffusion-webui");
        FinderEnv e = env();
        qint64 clock = 0;
        e.nowMs = [&clock] { return clock; };
        e.findCapMs = 1000000;
        e.totalCountCapMs = 5;
        e.touched = [&clock](const QString &) { clock += 1; };
        const FindResult result = find(e, all(e));
        QVERIFY(result.capped);
        QVERIFY(std::any_of(result.folders.begin(), result.folders.end(), [](const FoundFolder &f) { return f.countCapped; }));
    }

    // A diffusers repo (model_index.json) is not a model library: not found, and its files are not counted.
    void aDiffusersRepoIsSkipped() {
        QDir().mkpath(m_home + "/repo/vae");
        QDir().mkpath(m_home + "/repo/unet");
        QDir().mkpath(m_home + "/repo/checkpoints");
        touchFile(m_home + "/repo/model_index.json");
        touchFile(m_home + "/repo/vae/a.safetensors");
        touchFile(m_home + "/repo/checkpoints/a.safetensors");
        comfy(m_home + "/ComfyUI");
        QDir().mkpath(m_home + "/ComfyUI/models/loras/some-repo");
        touchFile(m_home + "/ComfyUI/models/loras/some-repo/model_index.json");
        touchFile(m_home + "/ComfyUI/models/loras/some-repo/w.safetensors");
        const FinderEnv e = env();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QCOMPARE(result.folders.first().counts.value("fugue/lora"), 3);
    }

    // An install folder ticked with the home not: only places inside what is ticked are stat'd.
    void aTickedProbeWithTheHomeUntickedTouchesNothingElseInTheHome() {
        comfy(m_home + "/ComfyUI");
        touchFile(m_home + "/secret/readme.txt");
        const FinderEnv e = env();
        const QList<FinderLocation> ticked = only(e, {"probe:" + m_home + "/ComfyUI/models"});
        QCOMPARE(ticked.size(), 1);
        m_touched.clear();
        const FindResult result = find(e, ticked);
        QCOMPARE(result.folders.size(), 1);
        QCOMPARE(result.folders.first().tool, QStringLiteral("ComfyUI")); // named by its neighbour, inside the install folder
        for (const QString &path : std::as_const(m_touched)) {
            QVERIFY2(!path.startsWith(m_home + "/secret"), qPrintable(path));
            QVERIFY2(path != m_home + "/python_embeded" && path != m_home + "/ComfyUI_windows_portable", qPrintable(path));
        }
    }

    void aWindowsDrivesInstallIsFoundFiveLevelsDown() {
        touchFile(m_mnt + "/c/Users/me/Documents/ComfyUI/models/checkpoints/a.safetensors");
        touchFile(m_mnt + "/c/Users/me/Documents/ComfyUI/models/loras/a.safetensors");
        const FinderEnv e = env();
        const FindResult result = find(e, all(e));
        QCOMPARE(result.folders.size(), 1);
        QCOMPARE(result.folders.first().models, 2);
    }
};

QTEST_GUILESS_MAIN(TestModelFolderFinder)
#include "test_model_folder_finder.moc"
