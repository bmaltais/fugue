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
#include <QTcpServer>
#include <QTcpSocket>
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
                    {"name": "opacity", "targets": ["shade.opacity"]},
                    {"name": "mode", "targets": ["shade.mode"]},
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

// #557: a model only a Switch's lazy side reaches, with the condition fixed to the other side: the run needs nothing that is missing.
const char *kSwitchedOff = R"({"fugue": 1,
 "requires": {"models": [{"resource": "fugue/image-model", "hash": "sha256:9999999999999999999999999999999999999999999999999999999999999999", "name": "m.safetensors", "source": "h.example/m.safetensors", "size": 7}]},
 "app": {"format": 1, "title": "Switched", "inputs": [{"role": "image", "targets": ["load.file"]}], "outputs": [{"node": "save"}]},
 "graph": {"nodes": {
   "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "inputs/pic.png"}},
   "gen": {"type": "fugue/generate-image", "version": "1.1.0", "inputs": {"prompt": "a cat", "model": {"resource": "fugue/image-model", "hash": "sha256:9999999999999999999999999999999999999999999999999999999999999999", "name": "m.safetensors"}}},
   "sw": {"type": "fugue/switch", "version": "1.0.0", "inputs": {"condition": false, "on_true": {"from": "gen.image"}, "on_false": {"from": "load.image"}}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "sw.value"}, "name": "app"}}}}})";

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

// A picture drawn by a provider: the app of the off-machine question (#404).
const char *kDraws = R"({"fugue": 1,
 "app": {"format": 1, "title": "Draws", "inputs": [{"name": "prompt", "targets": ["gen.prompt"]}], "outputs": [{"node": "save"}]},
 "graph": {"nodes": {
   "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a cat"}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}, "name": "app"}}}}})";

// #605: an app whose model (the INT8 file of Z-Image Turbo) is not on disk, with no source to get it from.
const char *kWantsInt8 = R"({"fugue": 1,
 "app": {"format": 1, "title": "Wants INT8", "inputs": [{"role": "image", "targets": ["load.file"]}], "outputs": [{"node": "save"}]},
 "graph": {"nodes": {
   "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": ""}},
   "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "sha256:9999999999999999999999999999999999999999999999999999999999999999", "name": "z_image_turbo_int8_convrot.safetensors"},
     "text_encoder": {"resource": "fugue/text-encoder", "hash": "sha256:8888888888888888888888888888888888888888888888888888888888888888", "name": "t.safetensors"},
     "vae": {"resource": "fugue/vae", "hash": "sha256:7777777777777777777777777777777777777777777777777777777777777777", "name": "v.safetensors"}}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, "name": "app"}}}}})";

// The same app, its model also a field of the app whose default holds the model the file names (binding writes that default back).
const char *kWantsInt8Field = R"({"fugue": 1,
 "app": {"format": 1, "title": "Wants INT8 field", "inputs": [{"role": "image", "targets": ["load.file"]},
   {"name": "unet", "targets": ["models.diffusion_model"], "default": {"resource": "fugue/diffusion-model", "hash": "sha256:9999999999999999999999999999999999999999999999999999999999999999", "name": "z_image_turbo_int8_convrot.safetensors"}}],
   "outputs": [{"node": "save"}]},
 "graph": {"nodes": {
   "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": ""}},
   "models": {"type": "fugue/load-models", "version": "1.0.0", "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "sha256:9999999999999999999999999999999999999999999999999999999999999999", "name": "z_image_turbo_int8_convrot.safetensors"},
     "text_encoder": {"resource": "fugue/text-encoder", "hash": "sha256:8888888888888888888888888888888888888888888888888888888888888888", "name": "t.safetensors"},
     "vae": {"resource": "fugue/vae", "hash": "sha256:7777777777777777777777777777777777777777777777777777777777777777", "name": "v.safetensors"}}},
   "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, "name": "app"}}}}})";

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

    // A Z-Image Turbo file (a header alone: Lumina 2's text projection and 16-channel patch embedding) in the model folder of the home.
    void zImage(const QString &name) const {
        const auto tensor = [](const char *shape) {
            return QByteArray("{\"dtype\": \"F16\", \"shape\": ") + shape + ", \"data_offsets\": [0, 0]}";
        };
        const QByteArray json = "{\"cap_embedder.1.weight\": " + tensor("[3840, 2560]") + ", \"x_embedder.weight\": " + tensor("[3840, 64]") + "}";
        QByteArray out;
        for (int i = 0; i < 8; ++i)
            out.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
        write(path("root/data/fugue/models/diffusion_models/" + name), out + json);
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

    // #605: the model is not on disk but the same weights are, in another quantization. Every message says where, and what to do; with
    // --use-on-disk the command goes on with that file, for this run only, saying which; the workflow file is not written.
    void aMissingModelWhoseSameWeightsAreOnDiskSaysSoAndUsesThemOnRequest() {
        write(path("work/wants-int8.fugue"), kWantsInt8);
        const QByteArray before = kWantsInt8;
        // No such file yet: nothing to suggest.
        QVERIFY(!fugue({"app", "run", "wants-int8.fugue", "--no-use-on-disk", "--input", "image=" + picture(), "--json"}).error().value("hint").toString().contains("--use-on-disk"));
        // The other two models are here by name (a check reads no file); only the diffusion model is the missing one.
        write(path("root/data/fugue/models/text_encoders/t.safetensors"), "abc");
        write(path("root/data/fugue/models/vae/v.safetensors"), "abc");
        zImage("z_image_turbo_bf16.safetensors");
        zImage("z_image_turbo_fp8_e4m3fn.safetensors");
        const QString file = path("root/data/fugue/models/diffusion_models/z_image_turbo_bf16.safetensors");

        const Outcome refused = fugue({"app", "run", "wants-int8.fugue", "--no-use-on-disk", "--input", "image=" + picture(), "--json"});
        QCOMPARE(refused.code(), QString("app_not_ready"));
        QVERIFY2(refused.error().value("hint").toString().contains(file), refused.out);
        QVERIFY(refused.error().value("hint").toString().contains("--use-on-disk"));

        // validate: the node's own warning (a missing model is a warning there) carries the same words.
        // The bundled Z-Image Turbo template is a graph that reaches the model; its files are not here, but the INT8 one has same weights.
        QVERIFY(QFile::copy(QString(FUGUE_SOURCE_DIR) + "/templates/z-image-turbo/workflow.fugue", path("work/zit.fugue")));
        const Outcome checked = fugue({"validate", "zit.fugue", "--no-use-on-disk", "--json"});
        QCOMPARE(checked.exitCode, 0);
        QStringList hints;
        for (const QJsonValue warning : checked.json().value("warnings").toArray())
            hints << warning.toObject().value("hint").toString();
        QVERIFY2(hints.join('\n').contains("set models.diffusion_model to it, or pass --use-on-disk"), checked.out);

        // --use-on-disk: the app is ready with the file, which the command prints; the workflow file is as it was.
        const Outcome ran = fugue({"app", "run", "wants-int8.fugue", "--use-on-disk", "--input", "image=" + picture(), "--json"});
        QVERIFY2(ran.code() != QString("app_not_ready"), ran.out + ran.err);
        QVERIFY2(ran.err.contains("--use-on-disk: z_image_turbo_int8_convrot.safetensors -> " + file.toUtf8()), ran.err);
        QVERIFY(ran.err.contains("same model, bf16"));
        QFile again(path("work/wants-int8.fugue"));
        QVERIFY(again.open(QIODevice::ReadOnly));
        QCOMPARE(again.readAll(), before);
        const Outcome valid = fugue({"validate", "zit.fugue", "--use-on-disk", "--json"});
        QCOMPARE(valid.exitCode, 0);
        for (const QJsonValue warning : valid.json().value("warnings").toArray())
            QVERIFY2(warning.toObject().value("input").toString() != "diffusion_model", valid.out);
    }

    // #618: the file is used by default (the setting use_on_disk is "use"), with no flag; --no-use-on-disk opts out, and "ask" is #605's
    // behaviour (a hint, no swap). Each swap is a line on stderr.
    void theFileIsUsedByDefaultUnlessOptedOutOrTheSettingIsAsk() {
        write(path("work/wants-int8.fugue"), kWantsInt8);
        write(path("root/data/fugue/models/text_encoders/t.safetensors"), "abc");
        write(path("root/data/fugue/models/vae/v.safetensors"), "abc");
        zImage("z_image_turbo_bf16.safetensors");
        const QString file = path("root/data/fugue/models/diffusion_models/z_image_turbo_bf16.safetensors");
        const QStringList run{"app", "run", "wants-int8.fugue", "--input", "image=" + picture(), "--json"};

        const Outcome bare = fugue(run);
        QVERIFY2(bare.code() != QString("app_not_ready"), bare.out + bare.err);
        QVERIFY2(bare.err.contains("z_image_turbo_int8_convrot.safetensors -> " + file.toUtf8()), bare.err);

        QCOMPARE(fugue(run + QStringList{"--no-use-on-disk"}).code(), QString("app_not_ready"));
        QVERIFY(fugue(run + QStringList{"--use-on-disk", "--no-use-on-disk"}).code() == QString("usage"));

        write(path("root/config/fugue/settings.json"), "{\"use_on_disk\": \"ask\"}");
        const Outcome asked = fugue(run);
        QCOMPARE(asked.code(), QString("app_not_ready"));
        QVERIFY(asked.error().value("hint").toString().contains("--use-on-disk"));
        const Outcome forced = fugue(run + QStringList{"--use-on-disk"}); // the flag still wins over "ask"
        QVERIFY2(forced.code() != QString("app_not_ready") && forced.err.contains(file.toUtf8()), forced.out + forced.err);
        QFile::remove(path("root/config/fugue/settings.json"));
    }

    // The binding writes a field's default over the node; the file chosen for --use-on-disk has to be put in after it, or the command says
    // it chose the file and runs with the missing model.
    void useOnDiskSurvivesAFieldWhoseDefaultIsTheMissingModel() {
        write(path("work/wants-field.fugue"), kWantsInt8Field);
        write(path("root/data/fugue/models/text_encoders/t.safetensors"), "abc");
        write(path("root/data/fugue/models/vae/v.safetensors"), "abc");
        zImage("z_image_turbo_bf16.safetensors");
        const Outcome ran = fugue({"app", "run", "wants-field.fugue", "--use-on-disk", "--input", "image=" + picture(), "--json"});
        QVERIFY2(ran.err.contains("--use-on-disk: z_image_turbo_int8_convrot.safetensors -> "), ran.err);
        QVERIFY2(ran.code() != QString("app_not_ready") && ran.code() != QString("model_missing"), ran.out + ran.err);
        const Outcome bare = fugue({"app", "run", "wants-field.fugue", "--use-on-disk", "--json"});
        QVERIFY2(bare.code() != QString("app_not_ready") && bare.code() != QString("model_missing"), bare.out + bare.err);
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

    void infoDropsAndSaysWhatTheWorkflowNoLongerHas() {
        const Outcome info = fugue({"app", "info", "shade-stale.fugue", "--json"});
        QCOMPARE(info.exitCode, 0);
        QStringList keys;
        QMap<QString, QJsonObject> rows;
        for (const QJsonValue row : info.data().value("inputs").toArray()) {
            keys << row.toObject().value("key").toString();
            rows.insert(keys.last(), row.toObject());
        }
        QCOMPARE(keys, (QStringList{"image", "opacity", "mode", "seed"})); // `old` is dropped, not listed
        QCOMPARE(rows.value("seed").value("default").toObject(), (QJsonObject{{"auto", "random"}}));
        // A group instance's exposed inputs take the inner inputs' type, range and choices, and the instance's own literal as the default.
        QCOMPARE(rows.value("opacity").value("type").toString(), QString("number"));
        QCOMPARE(rows.value("opacity").value("default").toDouble(), 0.25);
        QCOMPARE(rows.value("opacity").value("max").toDouble(), 1.0);
        QCOMPARE(rows.value("mode").value("type").toString(), QString("choice"));
        QVERIFY(rows.value("mode").value("choices").toArray().contains("multiply"));
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

    // 13.6: an --out outside the folders a run may write is refused before it is made; a package is said to be one, not "invalid JSON".
    void anOutFolderOutsideTheWriteRootsIsRefusedAndABundleIsNamed() {
        const QString elsewhere = path("elsewhere/out");
        const Outcome ran = fugue({"app", "run", "shade.fugue", "--input", "image=" + picture(), "--out", elsewhere, "--json"});
        QCOMPARE(ran.code(), QString("permission_denied"));
        QVERIFY(!QFileInfo::exists(elsewhere));
        write(path("work/x.fugueapp"), QByteArray("PK\x03\x04 and so on", 14));
        const Outcome bundle = fugue({"app", "info", "x.fugueapp", "--json"});
        QCOMPARE(bundle.code(), QString("invalid_document"));
        QVERIFY(bundle.error().value("message").toString().contains("package"));
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

    // #395, #581: with a paired helper configured, an app whose model is not on this machine does not stop at app_not_ready (the host
    // checks the files); with none it is not ready, as before. No node here asks a provider for the model, so no host is named.
    // The verdict's rules are tests/nodes/test_paired_placement.cpp's (the silent host is reported below), the run itself the
    // scheduler's (tests/engine/test_scheduler_providers.cpp).
    void aPairedHelperLetsAMissingModelThroughToTheRun() {
        write(path("work/sampled.fugue"), kSampled);
        const Outcome alone = fugue({"app", "info", "sampled.fugue", "--json"});
        QVERIFY(!alone.data().value("ready").toBool());
        QVERIFY(!alone.data().contains("runs_on"));
        QCOMPARE(fugue({"app", "run", "sampled.fugue", "--json"}).code(), QString("app_not_ready"));

        write(path("root/config/fugue/providers.json"),
              "{\"providers\": [{\"name\": \"gx10\", \"type\": \"fugue-helper\", \"address\": \"http://gx10.invalid:8787\", \"key\": \"k\"}]}");
        const Outcome paired = fugue({"app", "info", "sampled.fugue", "--json"});
        QVERIFY(!paired.data().contains("runs_on") && !paired.data().contains("silent_host"));
        // The run does not stop at app_not_ready (the model has no source here, or a source that cannot be used): the host checks.
        QVERIFY(fugue({"app", "run", "sampled.fugue", "--json"}).code() != QString("app_not_ready"));
        QByteArray unsourced = kSampled;
        unsourced.replace("\"source\": \"h.example/m.safetensors\", ", "");
        write(path("work/unsourced.fugue"), unsourced);
        const Outcome ran = fugue({"app", "run", "unsourced.fugue", "--json"});
        QVERIFY2(ran.code() != QString("app_not_ready"), ran.out + ran.err);
        QVERIFY(QFile::remove(path("root/config/fugue/providers.json")));
        QCOMPARE(fugue({"app", "run", "unsourced.fugue", "--json"}).code(), QString("app_not_ready")); // no helper: refused as before
    }

    // #557: no paired helper, and the model is held only by the side the Switch does not take: the run is not refused as not ready (it
    // would fail with its own model_missing if the other side were chosen, as it does here with the condition flipped).
    void aModelOnlyAnUnchosenSwitchSideNeedsDoesNotRefuseTheRun() {
        write(path("work/switched.fugue"), kSwitchedOff);
        QVERIFY(fugue({"app", "info", "switched.fugue", "--json"}).data().value("ready").toBool());
        const Outcome ran = fugue({"app", "run", "switched.fugue", "--input", "image=" + picture(), "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        QByteArray chosen = kSwitchedOff;
        chosen.replace("\"condition\": false", "\"condition\": true");
        write(path("work/chosen.fugue"), chosen);
        QCOMPARE(fugue({"app", "run", "chosen.fugue", "--input", "image=" + picture(), "--json"}).code(), QString("model_missing"));
    }

    // #491: the run is refused up front, naming the node, when the paired helper cannot take it: a node pinned to a provider on this
    // machine, or a node whose capability the host did not offer when it last answered.
    // 8.2, #404: an app that would send data to a service off this machine is refused like `fugue run`, and `--allow-remote` allows it.
    void anAppToAnOffMachineServiceNeedsTheYes() {
        template_("draws", "Draws", kDraws, "true");
        const QString module = path("root/data/fugue/providers/io.example.toy");
        QVERIFY(QDir().mkpath(module));
        QVERIFY(QFile::copy(QStringLiteral(FUGUE_FAKE_MODULE_PROVIDER), module + "/toy"));
        QFile(module + "/toy").setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        write(module + "/provider.json", "{\"manifest\": 1, \"id\": \"io.example.toy\", \"version\": \"0.1.0\", \"name\": \"Toy\", \"transport\": \"stdio\","
                                         " \"start\": {\"command\": [\"./toy\", \"--stdio\"]}, \"capabilities\": [\"image.generate\"]}");
        write(path("root/config/fugue/providers.json"), "{\"providers\": [{\"name\": \"Cloud\", \"type\": \"io.example.toy\", \"address\": \"https://cloud.example.com/v1\"}]}");
        const Outcome refused = fugue({"app", "run", "draws", "--json"});
        QCOMPARE(refused.code(), QString("consent_needed"));
        QVERIFY2(refused.error().value("message").toString().contains("Cloud (cloud.example.com)"), refused.out);
        QVERIFY(refused.error().value("hint").toString().contains("--allow-remote"));
        QCOMPARE(fugue({"app", "run", "draws", "--allow-remote", "Nowhere", "--json"}).code(), QString("unknown_provider"));
        QVERIFY(fugue({"app", "run", "draws", "--allow-remote", "Cloud", "--json"}).code() != QString("consent_needed"));
    }

    void aRunTheHostCannotTakeIsRefusedNamingTheNode() {
        QByteArray staged = kSampled;
        // What Load Models also needs to compile, as a run would.
        staged.replace("\"name\": \"m.safetensors\"}}},\n   \"save\"", "\"name\": \"m.safetensors\"},"
                       " \"text_encoder\": {\"resource\": \"fugue/text-encoder\", \"hash\": \"sha256:8888888888888888888888888888888888888888888888888888888888888888\", \"name\": \"t.safetensors\"},"
                       " \"vae\": {\"resource\": \"fugue/vae\", \"hash\": \"sha256:7777777777777777777777777777777777777777777777777777777777777777\", \"name\": \"v.safetensors\"}}},\n   \"save\"");
        staged.replace("\"save\":", "\"enc\": {\"type\": \"fugue/text-encode\", \"version\": \"1.0.0\", \"inputs\": {\"model\": {\"from\": \"models.model\"}, \"prompt\": \"a cat\"}},\n   \"show\": {\"type\": \"fugue/preview\", \"version\": \"1.0.0\", \"inputs\": {\"value\": {\"from\": \"enc.conditioning\"}}},\n   \"save\":");
        write(path("work/staged.fugue"), staged);
        QByteArray pinned = staged;
        pinned.replace("\"version\": \"1.0.0\", \"inputs\": {\"model\"", "\"version\": \"1.0.0\", \"provider\": \"desk\", \"inputs\": {\"model\"");
        write(path("work/pinned.fugue"), pinned);
        write(path("root/config/fugue/providers.json"),
              "{\"providers\": [{\"name\": \"desk\", \"type\": \"omarchy\"},"
              " {\"name\": \"gx10\", \"type\": \"fugue-helper\", \"address\": \"http://gx10.invalid:8787\", \"key\": \"k\"}]}");

        // Nothing is known of what the host offers and it does not answer: silent, and no host is named.
        const Outcome quiet = fugue({"app", "info", "staged.fugue", "--json"});
        QVERIFY(!quiet.data().contains("runs_on"));
        QCOMPARE(quiet.data().value("silent_host").toString(), QString("gx10"));
        // Pinned to a provider on this machine: refused, naming the node, with the prepare hint.
        const Outcome info = fugue({"app", "info", "pinned.fugue", "--json"});
        QVERIFY(!info.data().contains("runs_on"));
        QCOMPARE(info.data().value("host_problems").toArray().first().toObject().value("node").toString(), QString("enc"));
        const Outcome refused = fugue({"app", "run", "pinned.fugue", "--json"});
        QCOMPARE(refused.code(), QString("model_missing"));
        QVERIFY2(refused.error().value("message").toString().contains("Node enc is pinned to desk"), refused.out);
        QCOMPARE(refused.error().value("node").toString(), QString("enc"));
        QVERIFY(refused.error().value("hint").toString().contains("prepare"));

        // The host last offered images only: it could never serve a text.encode node.
        write(path("root/state/fugue/paired_helpers.json"),
              "{\"gx10\": {\"address\": \"http://gx10.invalid:8787\", \"engine\": \"sdcpp@gx10\", \"capabilities\": [\"image.generate\"]}}");
        QVERIFY(!fugue({"app", "info", "staged.fugue", "--json"}).data().contains("runs_on"));
        const Outcome noCapability = fugue({"app", "run", "staged.fugue", "--json"});
        QCOMPARE(noCapability.code(), QString("model_missing"));
        QVERIFY2(noCapability.error().value("message").toString().contains("did not offer text.encode when it last answered"), noCapability.out);
        write(path("root/state/fugue/paired_helpers.json"),
              "{\"gx10\": {\"address\": \"http://gx10.invalid:8787\", \"engine\": \"sdcpp@gx10\", \"capabilities\": [\"text.encode\"]}}");
        const Outcome remembered = fugue({"app", "info", "staged.fugue", "--json"}); // what it last said stands in for nothing: still silent
        QVERIFY(!remembered.data().contains("runs_on"));
        QCOMPARE(remembered.data().value("silent_host").toString(), QString("gx10"));

        // (A loopback address is no paired host, so 0.0.0.0, which reaches it.) The host offers it now (what was kept is stale): it is the host, and the run is not refused.
        QProcess host;
        const QString script = path("host.py");
        write(script, "import http.server, json, sys\n"
                      "class H(http.server.BaseHTTPRequestHandler):\n"
                      "    def do_POST(self):\n"
                      "        n = int(self.headers.get('Content-Length', 0)); self.rfile.read(n)\n"
                      "        if not self.path.endswith('/describe'):\n"
                      "            self.send_response(500); self.send_header('Content-Length', '0'); self.end_headers(); return\n"
                      "        body = json.dumps({'protocol': 1, 'engine': 'sdcpp', 'shares_filesystem': False, 'host': 'gx10', 'capabilities': ['text.encode']}).encode()\n"
                      "        self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)\n"
                      "    def log_message(self, *a): pass\n"
                      "http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()\n");
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();
        host.start("python3", {script, QString::number(port)});
        QVERIFY(host.waitForStarted());
        const auto up = [port] {
            QTcpSocket socket;
            socket.connectToHost(QHostAddress::LocalHost, port);
            return socket.waitForConnected(200);
        };
        QTRY_VERIFY_WITH_TIMEOUT(up(), 5000);
        write(path("root/config/fugue/providers.json"),
              QStringLiteral("{\"providers\": [{\"name\": \"desk\", \"type\": \"omarchy\"}, {\"name\": \"gx10\", \"type\": \"fugue-helper\", "
                             "\"address\": \"http://0.0.0.0:%1\", \"key\": \"k\"}]}").arg(port).toUtf8());
        write(path("root/state/fugue/paired_helpers.json"),
              QStringLiteral("{\"gx10\": {\"address\": \"http://0.0.0.0:%1\", \"engine\": \"sdcpp@gx10\", \"capabilities\": [\"image.generate\"]}}").arg(port).toUtf8());
        const Outcome freshInfo = fugue({"app", "info", "staged.fugue", "--json"});
        QCOMPARE(freshInfo.data().value("runs_on").toString(), QString("gx10"));
        QVERIFY(!freshInfo.data().contains("host_problems"));
        // The run is let through: it starts and goes to the host, whose job POSTs fail (HTTP 500), so it ends with a provider error and
        // not with the refusal (`model_missing`) it gets when no host takes the node.
        const Outcome through = fugue({"app", "run", "staged.fugue", "--json"});
        QVERIFY2(!through.code().isEmpty(), through.out + through.err);
        QVERIFY2(through.code() != QString("model_missing") && through.code() != QString("app_not_ready"), through.out + through.err);
        host.kill();
        host.waitForFinished(2000);

        // No paired helper configured: nothing changes (not ready, as before).
        write(path("root/config/fugue/providers.json"), "{\"providers\": [{\"name\": \"desk\", \"type\": \"omarchy\"}]}");
        QCOMPARE(fugue({"app", "run", "pinned.fugue", "--json"}).code(), QString("app_not_ready"));
        QVERIFY(QFile::remove(path("root/config/fugue/providers.json")));
        QVERIFY(QFile::remove(path("root/state/fugue/paired_helpers.json")));
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
