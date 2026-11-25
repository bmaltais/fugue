// `fugue app list|info|run` (#392, spec 4.9). Seam: the real `fugue` binary as a subprocess with a root of its own (FUGUE_HOME),
// an empty HOME, a folder of templates of the test's own (FUGUE_TEMPLATES) and no GPU helper. The app is load-image, invert and
// save-image, so the whole path (the binding, the queue, the worker, the Save node writing into --out) runs for real on the CPU.
// The block's reading and binding are tests/engine/test_app_binding.cpp's; this is the command: what it lists, the codes it
// refuses with, and the files it prints.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/share_package.h"

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QJsonObject error() const { return json().value("errors").toArray().at(0).toObject(); }
    QString code() const { return error().value("code").toString(); }
};

const char *kInvert = R"({"fugue": 1,
 "app": {"format": 1, "title": "Invert", "description": "Inverts a picture.",
         "inputs": [{"role": "image", "targets": ["load.file"], "label": "Picture"},
                    {"name": "name", "targets": ["save.name"], "label": "Name"},
                    {"name": "amount", "targets": ["pad.value"], "min": 1, "max": 5, "default": 2}],
         "outputs": [{"node": "save", "label": "Result"}]},
 "graph": {"nodes": {
   "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": ""}},
   "pad": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 1}},
   "invert": {"type": "fugue/invert", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "invert.image"}, "name": "app"}}}}})";

// The same graph with a node of a pack that is not installed, and no source to get it from.
const char *kNeedsPack = R"({"fugue": 1, "requires": {"packs": {"io.example.tools": {"version": "1.0.0"}}},
 "app": {"format": 1, "title": "Needs a pack", "inputs": [], "outputs": [{"node": "save", "label": "Result"}]},
 "graph": {"nodes": {
   "other": {"type": "io.example.tools/tool", "version": "1.0.0", "inputs": {}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "other.image"}}}}}})";

// A picture through a library group instance (spec 5.4): the Blend's `opacity` and `mode` are exposed by the group, so the app's
// inputs bind to a group's exposed inputs, with the inner spec (number 0..1, a choice). The text is split so a second app can
// add what the workflow does not have (see kShadeStale).
#define SHADE_GROUP R"(,
 "groups": {"local/shade": {"version": "1.0.0", "name": "Shade",
   "inputs": [{"name": "image", "targets": ["mix.base", "mix.top"]},
              {"name": "opacity", "targets": ["mix.opacity"]},
              {"name": "mode", "targets": ["mix.mode"]}],
   "outputs": [{"name": "image", "from": "mix.image"}],
   "nodes": {"mix": {"type": "fugue/blend", "version": "1.0.0", "inputs": {}}}}}})"
#define SHADE_NODES R"(
   "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": ""}},
   "shade": {"type": "local/shade", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, "opacity": 0.25}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "shade.image"}, "name": "app"}})"

const char *kShade = R"({"fugue": 1,
 "app": {"format": 1, "title": "Shade",
         "inputs": [{"role": "image", "targets": ["load.file"]},
                    {"name": "opacity", "targets": ["shade.opacity"]},
                    {"name": "mode", "targets": ["shade.mode"]}],
         "outputs": [{"node": "save"}]},
 "graph": {"nodes": {)" SHADE_NODES R"(}})" SHADE_GROUP;

// The same app with an input and an output the workflow does not have (`gone`), and a sampler whose seed is the workflow's own
// {"auto": "random"}.
const char *kShadeStale = R"({"fugue": 1,
 "app": {"format": 1, "title": "Shade",
         "inputs": [{"role": "image", "targets": ["load.file"]},
                    {"name": "old", "targets": ["shade.gone"]},
                    {"role": "seed", "targets": ["sample.seed"]}],
         "outputs": [{"node": "save"}, {"node": "gone", "label": "Lost"}]},
 "graph": {"nodes": {
   "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"seed": {"auto": "random"}}},)" SHADE_NODES R"(}})" SHADE_GROUP;

// An app whose picture and another file (no image) are in its folder, and whose model has a source (app export).
const char *kSampled = R"({"fugue": 1,
 "requires": {"models": [{"resource": "fugue/diffusion-model", "hash": "sha256:9999999999999999999999999999999999999999999999999999999999999999", "name": "m.safetensors", "source": "h.example/m.safetensors", "size": 7}]},
 "app": {"format": 1, "title": "Sampled", "inputs": [{"role": "image", "targets": ["load.file"]}, {"name": "clip", "targets": ["clip.file"]}],
         "outputs": [{"node": "save"}]},
 "graph": {"nodes": {
   "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "inputs/pic.png"}},
   "clip": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "inputs/clip.bin"}},
   "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "sha256:9999999999999999999999999999999999999999999999999999999999999999", "name": "m.safetensors"}}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}}}}})";

// A second picture that is optional (#484): it is the mask of a Color Match, and left out its Load Image is bypassed, so the mask is absent.
const char *kOptionalMask = R"({"fugue": 1,
 "app": {"format": 1, "title": "Match", "inputs": [{"role": "image", "targets": ["load.file"]},
                                                  {"role": "image", "name": "region", "targets": ["region.file"], "optional": true}],
         "outputs": [{"node": "save"}]},
 "graph": {"nodes": {
   "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": ""}},
   "region": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "old.png"}},
   "match": {"type": "fugue/color-match", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, "reference": {"from": "load.image"}, "mask": {"from": "region.image"}}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "match.image"}, "name": "app"}}}}})";

const char *kPlain = R"({"fugue": 1, "graph": {"nodes": {"n": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 1}}}}})";

} // namespace

class TestCliApp : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    Outcome fugue(const QStringList &args) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "FUGUE_PROJECT",
                                     "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
            env.remove(QString::fromLatin1(variable));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_HOME", path("root"));
        env.insert("FUGUE_TEMPLATES", path("templates"));
        env.insert("FUGUE_HELPER", "");
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(path("work"));
        process.start(FUGUE_BINARY, args);
        Outcome outcome;
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            return outcome;
        }
        outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    void write(const QString &file, const QByteArray &bytes) const {
        QVERIFY(QDir().mkpath(QFileInfo(file).absolutePath()));
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(bytes);
    }

    // `app` is what template.json says of it: "true", "false" or nothing.
    void template_(const QString &id, const QString &title, const QByteArray &workflow, const QByteArray &app = {}) const {
        write(path("templates/" + id + "/template.json"),
              "{\"title\": \"" + title.toUtf8() + "\"" + (app.isEmpty() ? QByteArray() : ", \"app\": " + app) + "}");
        write(path("templates/" + id + "/workflow.fugue"), workflow);
    }

    // Under the home folder: the one place a run may read from besides its own (13.6).
    QString picture() const { return path("home/pictures/in.png"); }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(path("home")));
        QVERIFY(QDir().mkpath(path("work")));
        // A 1x1 PNG: a real picture through the real loader.
        write(picture(), QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="));
        template_("invert", "Invert template", kInvert, R"({"summary": "Flip the colors.", "kind": "image"})");
        template_("hidden", "Hidden", kInvert, "false");
        template_("plain", "Plain", kPlain);
        write(path("work/needs-pack.fugue"), kNeedsPack);
        write(path("work/invert.fugue"), kInvert);
        write(path("work/optional.fugue"), kOptionalMask);
        write(path("work/shade.fugue"), kShade);
        write(path("work/shade-stale.fugue"), kShadeStale);
    }

    void listShowsTheAppsAndNotThePlainTemplates() {
        const Outcome listed = fugue({"app", "list", "--json"});
        QCOMPARE(listed.exitCode, 0);
        QStringList ids;
        for (const QJsonValue app : listed.data().value("apps").toArray())
            ids << app.toObject().value("id").toString();
        ids.sort();
        QCOMPARE(ids, (QStringList{"invert"}));
        QCOMPARE(listed.data().value("apps").toArray().at(0).toObject().value("title").toString(), QString("Invert"));
    }

    // The catalog's flag (#387, 11.10) is what `app list` lists by; `template list` carries it with the card's words.
    void templateListCarriesTheAppFlagAndAppListFollowsIt() {
        const Outcome listed = fugue({"template", "list", "--json"});
        QCOMPARE(listed.exitCode, 0);
        QMap<QString, QJsonValue> app;
        for (const QJsonValue entry : listed.data().value("templates").toArray())
            app.insert(entry.toObject().value("id").toString(), entry.toObject().value("app"));
        QCOMPARE(app.value("invert").toObject(),
                 (QJsonObject{{"title", "Invert"}, {"summary", "Flip the colors."}, {"kind", "image"}}));
        QCOMPARE(app.value("hidden"), QJsonValue(false)); // app: false keeps a workflow with a block out of the Apps
        QCOMPARE(app.value("plain"), QJsonValue(false));
        QStringList ids;
        for (const QJsonValue entry : fugue({"app", "list", "--json"}).data().value("apps").toArray())
            ids << entry.toObject().value("id").toString();
        QCOMPARE(ids, (QStringList{"invert"}));
        QCOMPARE(fugue({"app", "info", "hidden", "--json"}).exitCode, 0); // still an app by its block, by name
    }

    void infoGivesTheInputsAndOutputsOfATemplateOrAFile() {
        for (const QString &ref : {QString("invert"), QString("invert.fugue")}) {
            const Outcome info = fugue({"app", "info", ref, "--json"});
            QCOMPARE(info.exitCode, 0);
            const QJsonObject data = info.data();
            QVERIFY(data.value("ready").toBool());
            QCOMPARE(data.value("title").toString(), QString("Invert"));
            const QJsonArray inputs = data.value("inputs").toArray();
            QCOMPARE(inputs.size(), 3);
            const QJsonObject picture = inputs.at(0).toObject();
            QCOMPARE(picture.value("key").toString(), QString("image"));
            QCOMPARE(picture.value("type").toString(), QString("image"));
            QCOMPARE(picture.value("label").toString(), QString("Picture"));
            QVERIFY(picture.value("required").toBool());
            const QJsonObject name = inputs.at(1).toObject();
            QCOMPARE(name.value("type").toString(), QString("text"));
            QCOMPARE(name.value("default").toString(), QString("app")); // the workflow's own value
            QVERIFY(!name.value("required").toBool());
            const QJsonObject amount = inputs.at(2).toObject();
            QCOMPARE(amount.value("type").toString(), QString("number"));
            QCOMPARE(amount.value("default").toInt(), 2); // the block's default
            QCOMPARE(amount.value("min").toInt(), 1);
            QCOMPARE(amount.value("max").toInt(), 5);
            QCOMPARE(data.value("outputs").toArray().at(0).toObject().value("label").toString(), QString("Result"));
        }
    }

    // An input bound to a group instance's exposed input has the inner input's type, range and choices; what the graph no
    // longer has is dropped and said as `app_stale` (spec 4.9), never listed; a seed left to the engine shows as it is.
    void infoTypesAGroupInstanceInput() {
        const Outcome info = fugue({"app", "info", "shade.fugue", "--json"});
        QCOMPARE(info.exitCode, 0);
        QMap<QString, QJsonObject> rows;
        for (const QJsonValue row : info.data().value("inputs").toArray())
            rows.insert(row.toObject().value("key").toString(), row.toObject());
        QCOMPARE(rows.value("opacity").value("type").toString(), QString("number"));
        QCOMPARE(rows.value("opacity").value("default").toDouble(), 0.25);
        QCOMPARE(rows.value("opacity").value("min").toDouble(), 0.0);
        QCOMPARE(rows.value("opacity").value("max").toDouble(), 1.0);
        QCOMPARE(rows.value("mode").value("type").toString(), QString("choice"));
        QVERIFY(rows.value("mode").value("choices").toArray().contains("multiply"));
        QVERIFY(!info.json().contains("warnings"));
    }

    void infoDropsAndSaysWhatTheWorkflowNoLongerHas() {
        const Outcome info = fugue({"app", "info", "shade-stale.fugue", "--json"});
        QCOMPARE(info.exitCode, 0);
        QStringList keys;
        for (const QJsonValue row : info.data().value("inputs").toArray())
            keys << row.toObject().value("key").toString();
        QCOMPARE(keys, (QStringList{"image", "seed"})); // `old` is dropped, not listed
        QCOMPARE(info.data().value("inputs").toArray().at(1).toObject().value("default").toObject(), (QJsonObject{{"auto", "random"}}));
        QCOMPARE(info.data().value("outputs").toArray().size(), 1); // `gone` is dropped, not listed
        QStringList codes, said;
        for (const QJsonValue w : info.json().value("warnings").toArray()) {
            codes << w.toObject().value("code").toString();
            said << w.toObject().value("message").toString();
        }
        QCOMPARE(codes, (QStringList{"app_stale", "app_stale", "app_stale"}));
        QVERIFY2(said.join(' ').contains("shade.gone"), qPrintable(said.join(' ')));
        QVERIFY2(said.join(' ').contains("output gone"), qPrintable(said.join(' ')));
    }

    void runTakesANumberForAGroupInstanceInput() {
        const QString out = path("work/shade-results");
        const Outcome ran = fugue({"app", "run", "shade.fugue", "--input", "image=" + picture(), "--input", "opacity=0.7", "--input", "mode=multiply",
                                   "--out", out, "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        QCOMPARE(ran.data().value("files").toArray().size(), 1);
        QCOMPARE(fugue({"app", "run", "shade.fugue", "--input", "image=" + picture(), "--input", "opacity=lots", "--json"}).code(), QString("usage"));
    }

    void infoSaysWhenTheAppIsNotReady() {
        const Outcome info = fugue({"app", "info", "needs-pack.fugue", "--json"});
        QCOMPARE(info.exitCode, 0);
        QVERIFY(!info.data().value("ready").toBool());
        QVERIFY(info.data().contains("needs"));
    }

    void aWorkflowWithNoBlockOrNoSuchAppIsRefused() {
        write(path("work/plain.fugue"), kPlain);
        QCOMPARE(fugue({"app", "info", "plain.fugue", "--json"}).code(), QString("no_app"));
        QCOMPARE(fugue({"app", "info", "plain", "--json"}).code(), QString("no_app")); // a template with no block
        const Outcome none = fugue({"app", "run", "nothing", "--json"});
        QCOMPARE(none.code(), QString("file_not_found"));
        QVERIFY(none.error().value("valid").toArray().contains("invert"));
    }

    void runRefusesWhatItCannotBindBeforeAnythingRuns() {
        QCOMPARE(fugue({"app", "run", "invert", "--json"}).code(), QString("missing_input"));
        const Outcome unknown = fugue({"app", "run", "invert", "--input", "color=red", "--json"});
        QCOMPARE(unknown.code(), QString("unknown_input"));
        QVERIFY(unknown.error().value("valid").toArray().contains("amount"));
        QCOMPARE(fugue({"app", "run", "invert", "--input", "image=" + path("work/none.png"), "--json"}).code(), QString("file_not_found"));
        QCOMPARE(fugue({"app", "run", "invert", "--input", "image=" + picture(), "--input", "amount=lots", "--json"}).code(), QString("usage"));
        QCOMPARE(fugue({"app", "run", "needs-pack.fugue", "--json"}).code(), QString("app_not_ready"));
    }

    // `app export` (#394, spec 11.5): the bundle is the package's zip, read back with the package reader.
    void exportWritesABundleWithItsSampleFilesAndRefusesWhatItCannotCarry() {
        const QByteArray sampled = kSampled;
        template_("sampled", "Sampled", sampled);
        write(path("templates/sampled/inputs/pic.png"), QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="));
        write(path("templates/sampled/inputs/clip.bin"), "clip bytes, no picture");

        const QString out = path("work/sampled");
        const Outcome wrote = fugue({"app", "export", "sampled", out, "--json"});
        QVERIFY2(wrote.exitCode == 0, wrote.out + wrote.err);
        QCOMPARE(wrote.data().value("file").toString(), out + ".fugueapp"); // its own extension, added
        QCOMPARE(wrote.data().value("packs").toArray().size(), 0);
        QStringList files;
        for (const QJsonValue file : wrote.data().value("files").toArray())
            files << file.toString();
        files.sort();
        QCOMPARE(files, (QStringList{"clip.bin", "pic.png"}));
        // Read back with Fugue's own reader (the CI containers have no unzip).
        QVERIFY(QDir().mkpath(path("work/unpacked")));
        const fugue::Result<QString> unpacked = fugue::unpackSharePackage(out + ".fugueapp", path("work/unpacked"));
        QVERIFY2(unpacked, unpacked ? "" : qPrintable(unpacked.error().message));
        for (const char *entry : {"inputs/clip.bin", "inputs/pic.png", "workflow.fugue"})
            QVERIFY2(QFileInfo::exists(unpacked.value() + "/" + entry), entry);

        // Never over a file; a file as the app, and the codes of the rest.
        QCOMPARE(fugue({"app", "export", "sampled", out, "--json"}).code(), QString("permission_denied"));
        QVERIFY(fugue({"app", "export", path("work/invert.fugue"), path("work/from-file.fugueapp"), "--json"}).exitCode == 0);
        QCOMPARE(fugue({"app", "export", "plain", path("work/p.fugueapp"), "--json"}).code(), QString("no_app"));
        QCOMPARE(fugue({"app", "export", "nothing", path("work/n.fugueapp"), "--json"}).code(), QString("file_not_found"));
        QCOMPARE(fugue({"app", "export", "sampled", "--json"}).code(), QString("usage")); // the file is missing

        // A pack that is not installed and has no source: the receiver would be stuck, so it is refused, naming the pack.
        const Outcome refused = fugue({"app", "export", "needs-pack.fugue", path("work/refused.fugueapp"), "--json"});
        QCOMPARE(refused.code(), QString("app_not_shareable"));
        QVERIFY(refused.error().value("message").toString().contains("Tools 1.0.0"));
        QVERIFY(!QFileInfo::exists(path("work/refused.fugueapp")));

        // A model with no source is refused too, and named.
        QByteArray unsourced = sampled;
        unsourced.replace(", \"source\": \"h.example/m.safetensors\", \"size\": 7", "");
        write(path("work/unsourced.fugue"), unsourced);
        const Outcome noSource = fugue({"app", "export", path("work/unsourced.fugue"), path("work/u.fugueapp"), "--json"});
        QCOMPARE(noSource.code(), QString("app_not_shareable"));
        QVERIFY(noSource.error().value("message").toString().contains("m.safetensors"));
    }

    // #484: an optional picture is listed as optional, with no default (the file its node holds is not one), and may be left out.
    void anOptionalPictureMayBeLeftOut() {
        const Outcome info = fugue({"app", "info", path("work/optional.fugue"), "--json"});
        QVERIFY2(info.exitCode == 0, info.out + info.err);
        const QJsonObject region = info.data().value("inputs").toArray().at(1).toObject();
        QCOMPARE(region.value("key").toString(), QString("region"));
        QVERIFY(region.value("optional").toBool());
        QVERIFY(!region.value("required").toBool());
        QVERIFY(!region.contains("default"));
        QVERIFY(info.data().value("inputs").toArray().at(0).toObject().value("required").toBool());
        const Outcome text = fugue({"app", "info", path("work/optional.fugue")});
        QVERIFY2(QString::fromUtf8(text.out).contains("optional"), text.out + text.err);

        const QString out = path("work/optional-results");
        const Outcome left = fugue({"app", "run", path("work/optional.fugue"), "--input", "image=" + picture(), "--out", out, "--json"});
        QVERIFY2(left.exitCode == 0, left.out + left.err);
        QCOMPARE(left.data().value("files").toArray().size(), 1);
        const Outcome given = fugue({"app", "run", path("work/optional.fugue"), "--input", "image=" + picture(), "--input", "region=" + picture(),
                                     "--out", out, "--json"});
        QVERIFY2(given.exitCode == 0, given.out + given.err);
        // The required one left out still fails before anything runs.
        const Outcome refused = fugue({"app", "run", path("work/optional.fugue"), "--json"});
        QCOMPARE(refused.code(), QString("missing_input"));
    }

    void runPrintsTheFilesTheOutputNodeWroteInOut() {
        write(path("work/name.txt"), "mine\n");
        const QString out = path("work/results");
        const Outcome ran = fugue({"app", "run", "invert", "--input", "image=" + picture(), "--input", "name=@" + path("work/name.txt"), "--input", "amount=3",
                                   "--out", out, "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        const QJsonArray files = ran.data().value("files").toArray();
        QCOMPARE(files.size(), 1);
        const QString file = files.at(0).toString();
        QVERIFY2(file.startsWith(out + "/mine"), qPrintable(file));
        QVERIFY(QFile::exists(file));
        const QJsonObject result = ran.data().value("outputs").toArray().at(0).toObject();
        QCOMPARE(result.value("label").toString(), QString("Result"));
        QCOMPARE(result.value("files").toArray().at(0).toString(), file);

        // Without --json the text is the paths, one per line, for a script to read; a file path is as good as an id.
        const Outcome text = fugue({"app", "run", path("work/invert.fugue"), "--input", "image=" + picture(), "--out", out});
        QVERIFY2(text.exitCode == 0, text.out + text.err);
        QVERIFY(QString::fromUtf8(text.out).trimmed().startsWith(out + "/app"));
        // Nothing was written to the home folder.
        QCOMPARE(QDir(path("home")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{"pictures"});
    }
};

QTEST_GUILESS_MAIN(TestCliApp)
#include "test_cli_app.moc"
